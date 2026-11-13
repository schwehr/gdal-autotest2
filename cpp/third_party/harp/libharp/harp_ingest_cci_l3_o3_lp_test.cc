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
// Unit tests for ESA CCI L3 O3 limb profile ingestion modules
// (libharp/harp-ingest-cci_l3_o3_lp.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-cci_l3_o3_lp.c

#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_coda/libcoda/coda.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::DoubleEq;
using ::testing::ElementsAre;

TEST(HarpIngestCciL3O3LpTest, MzmIngestSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_OZONE_L3_LP_MZM");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());

  // [num_time=2, num_vertical=2, num_latitude=3].
  constexpr long kTimeDims[] = {2};
  constexpr double kTimes[] = {100.0, 200.0};
  constexpr long kLatDims[] = {3};
  constexpr double kLats[] = {-30.0, 0.0, 30.0};
  constexpr long kVertDims[] = {2};
  constexpr double kAlts[] = {20.0, 30.0};
  constexpr double kPress[] = {50.0, 10.0};

  constexpr long kDataDims[] = {2, 2, 3};
  // Input [t][v][lat] -> transposed {0, 2, 1} to [t][lat][v].
  constexpr double kVmr[] = {
      1.0, 2.0, 3.0, 4.0,  5.0,  6.0,  // t=0: v=0 (1,2,3), v=1 (4,5,6)
      7.0, 8.0, 9.0, 10.0, 11.0, 12.0  // t=1: v=0 (7,8,9), v=1 (10,11,12)
  };
  constexpr double kConc[] = {
      10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0, 110.0, 120.0,
  };

  ASSERT_TRUE(prod.AddField(prod.root(), "time",
                            prod.MakeDoubleArray(kTimeDims, kTimes)));
  ASSERT_TRUE(prod.AddField(prod.root(), "latitude_centers",
                            prod.MakeDoubleArray(kLatDims, kLats)));
  ASSERT_TRUE(prod.AddField(prod.root(), "approximate_altitude",
                            prod.MakeDoubleArray(kVertDims, kAlts)));
  ASSERT_TRUE(prod.AddField(prod.root(), "air_pressure",
                            prod.MakeDoubleArray(kVertDims, kPress)));
  ASSERT_TRUE(prod.AddField(prod.root(), "ozone_mixing_ratio",
                            prod.MakeDoubleArray(kDataDims, kVmr)));
  // Upstream CODA field name typo: "ozone_mole_concentation".
  ASSERT_TRUE(prod.AddField(prod.root(), "ozone_mole_concentation",
                            prod.MakeDoubleArray(kDataDims, kConc)));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(3L, res.dimensions[harp_dimension_latitude]);
  EXPECT_EQ(0L, res.dimensions[harp_dimension_longitude]);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_vertical]);

  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(100.0, 200.0));
  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(-30.0, 0.0, 30.0));
  EXPECT_THAT(res.variables["altitude"].double_values, ElementsAre(20.0, 30.0));
  EXPECT_THAT(res.variables["pressure"].double_values, ElementsAre(50.0, 10.0));
  EXPECT_THAT(res.variables["O3_volume_mixing_ratio"].double_values,
              ElementsAre(1.0, 4.0, 2.0, 5.0, 3.0, 6.0, 7.0, 10.0, 8.0, 11.0,
                          9.0, 12.0));
  EXPECT_THAT(res.variables["O3_number_density"].double_values,
              ElementsAre(10.0, 40.0, 20.0, 50.0, 30.0, 60.0, 70.0, 100.0, 80.0,
                          110.0, 90.0, 120.0));
}

TEST(HarpIngestCciL3O3LpTest, MmzmIngestSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_OZONE_L3_LP_MMZM");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());

  ASSERT_TRUE(prod.AddAttribute(prod.root(), "year", prod.MakeString("2020")));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "month", prod.MakeString("05")));

  // [num_vertical=2, num_latitude=3], num_time=1.
  constexpr long kLatDims[] = {3};
  constexpr double kLats[] = {-20.0, 0.0, 20.0};
  constexpr long kVertDims[] = {2};
  constexpr double kAlts[] = {25.0, 35.0};
  constexpr double kPress[] = {30.0, 5.0};
  constexpr long kDataDims[] = {2, 3};
  constexpr double kVmr[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  constexpr double kConc[] = {10.0, 20.0, 30.0, 40.0, 50.0, 60.0};
  constexpr double kRelErr[] = {10.0, 10.0, 20.0, 20.0, 50.0, 50.0};

  ASSERT_TRUE(prod.AddField(prod.root(), "latitude_centers",
                            prod.MakeDoubleArray(kLatDims, kLats)));
  ASSERT_TRUE(prod.AddField(prod.root(), "approximate_altitude",
                            prod.MakeDoubleArray(kVertDims, kAlts)));
  ASSERT_TRUE(prod.AddField(prod.root(), "air_pressure",
                            prod.MakeDoubleArray(kVertDims, kPress)));
  ASSERT_TRUE(prod.AddField(prod.root(), "merged_ozone_vmr",
                            prod.MakeDoubleArray(kDataDims, kVmr)));
  ASSERT_TRUE(prod.AddField(prod.root(), "merged_ozone_concentration",
                            prod.MakeDoubleArray(kDataDims, kConc)));
  ASSERT_TRUE(prod.AddField(prod.root(), "uncertainty_of_merged_ozone",
                            prod.MakeDoubleArray(kDataDims, kRelErr)));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(1L, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(3L, res.dimensions[harp_dimension_latitude]);
  EXPECT_EQ(0L, res.dimensions[harp_dimension_longitude]);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_vertical]);

  double expected_dt = 0.0;
  ASSERT_EQ(0,
            coda_time_string_to_double("yyyyMMdd", "20200501", &expected_dt));
  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(DoubleEq(expected_dt)));
  // Upstream MMZM registers read_latitude for longitude as well as latitude.
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(-20.0, 0.0, 20.0));
  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(-20.0, 0.0, 20.0));
  EXPECT_THAT(res.variables["altitude"].double_values, ElementsAre(25.0, 35.0));
  EXPECT_THAT(res.variables["pressure"].double_values, ElementsAre(30.0, 5.0));
  EXPECT_THAT(res.variables["O3_volume_mixing_ratio"].double_values,
              ElementsAre(1.0, 4.0, 2.0, 5.0, 3.0, 6.0));
  EXPECT_THAT(res.variables["O3_volume_mixing_ratio_uncertainty"].double_values,
              ElementsAre(DoubleEq(0.1), DoubleEq(0.8), DoubleEq(0.2),
                          DoubleEq(2.5), DoubleEq(0.6), DoubleEq(3.0)));
  EXPECT_THAT(res.variables["O3_number_density"].double_values,
              ElementsAre(10.0, 40.0, 20.0, 50.0, 30.0, 60.0));
  EXPECT_THAT(res.variables["O3_number_density_uncertainty"].double_values,
              ElementsAre(DoubleEq(1.0), DoubleEq(8.0), DoubleEq(2.0),
                          DoubleEq(25.0), DoubleEq(6.0), DoubleEq(30.0)));
}

TEST(HarpIngestCciL3O3LpTest, MsmmIngestSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_OZONE_L3_LP_MSMM");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());

  // [num_time=2, num_vertical=2, num_longitude=2, num_latitude=2].
  constexpr long kTimeDims[] = {2};
  constexpr double kTimes[] = {10.0, 20.0};
  constexpr long kLonDims[] = {2};
  constexpr double kLons[] = {-90.0, 90.0};
  constexpr long kLatDims[] = {2};
  constexpr double kLats[] = {-45.0, 45.0};
  constexpr long kVertDims[] = {2};
  constexpr double kAlts[] = {15.0, 25.0};
  constexpr double kPress[] = {100.0, 20.0};

  constexpr long kDataDims[] = {2, 2, 2, 2};
  constexpr double kVmr[] = {
      1, 2,  3,  4,  5,  6,  7,  8,  // t=0
      9, 10, 11, 12, 13, 14, 15, 16  // t=1
  };
  constexpr double kConc[] = {
      10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160,
  };
  constexpr double kRelErr[] = {
      10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
  };

  ASSERT_TRUE(prod.AddField(prod.root(), "time",
                            prod.MakeDoubleArray(kTimeDims, kTimes)));
  ASSERT_TRUE(prod.AddField(prod.root(), "longitude_centers",
                            prod.MakeDoubleArray(kLonDims, kLons)));
  ASSERT_TRUE(prod.AddField(prod.root(), "latitude_centers",
                            prod.MakeDoubleArray(kLatDims, kLats)));
  ASSERT_TRUE(prod.AddField(prod.root(), "approximate_altitude",
                            prod.MakeDoubleArray(kVertDims, kAlts)));
  ASSERT_TRUE(prod.AddField(prod.root(), "air_pressure",
                            prod.MakeDoubleArray(kVertDims, kPress)));
  ASSERT_TRUE(prod.AddField(prod.root(), "merged_ozone_vmr",
                            prod.MakeDoubleArray(kDataDims, kVmr)));
  ASSERT_TRUE(prod.AddField(prod.root(), "merged_ozone_concentration",
                            prod.MakeDoubleArray(kDataDims, kConc)));
  ASSERT_TRUE(prod.AddField(prod.root(), "uncertainty_of_merged_ozone",
                            prod.MakeDoubleArray(kDataDims, kRelErr)));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_latitude]);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_longitude]);
  EXPECT_EQ(2L, res.dimensions[harp_dimension_vertical]);

  EXPECT_THAT(res.variables["datetime"].double_values, ElementsAre(10.0, 20.0));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(-90.0, 90.0));
  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(-45.0, 45.0));
  EXPECT_THAT(res.variables["altitude"].double_values, ElementsAre(15.0, 25.0));
  EXPECT_THAT(res.variables["pressure"].double_values,
              ElementsAre(100.0, 20.0));
  // Transposed {0, 3, 2, 1}: [t][lat][lon][v].
  EXPECT_THAT(
      res.variables["O3_volume_mixing_ratio"].double_values,
      ElementsAre(1, 5, 3, 7, 2, 6, 4, 8, 9, 13, 11, 15, 10, 14, 12, 16));
  EXPECT_THAT(
      res.variables["O3_volume_mixing_ratio_uncertainty"].double_values,
      ElementsAre(DoubleEq(0.1), DoubleEq(0.5), DoubleEq(0.3), DoubleEq(0.7),
                  DoubleEq(0.2), DoubleEq(0.6), DoubleEq(0.4), DoubleEq(0.8),
                  DoubleEq(0.9), DoubleEq(1.3), DoubleEq(1.1), DoubleEq(1.5),
                  DoubleEq(1.0), DoubleEq(1.4), DoubleEq(1.2), DoubleEq(1.6)));
  EXPECT_THAT(res.variables["O3_number_density"].double_values,
              ElementsAre(10, 50, 30, 70, 20, 60, 40, 80, 90, 130, 110, 150,
                          100, 140, 120, 160));
  EXPECT_THAT(
      res.variables["O3_number_density_uncertainty"].double_values,
      ElementsAre(DoubleEq(1), DoubleEq(5), DoubleEq(3), DoubleEq(7),
                  DoubleEq(2), DoubleEq(6), DoubleEq(4), DoubleEq(8),
                  DoubleEq(9), DoubleEq(13), DoubleEq(11), DoubleEq(15),
                  DoubleEq(10), DoubleEq(14), DoubleEq(12), DoubleEq(16)));
}

TEST(HarpIngestCciL3O3LpTest, InitDimensionErrorsAllModules) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  struct DimErrCase {
    std::string_view module_name;
    std::string_view field_name;
    long wrong_dims[2];
    int expected_errno;
  };
  constexpr DimErrCase kMissingCases[] = {
      {.module_name = "ESACCI_OZONE_L3_LP_MZM",
       .field_name = "ozone_mole_concentation",
       .wrong_dims = {1, 2},
       .expected_errno = HARP_ERROR_INGESTION},
      {.module_name = "ESACCI_OZONE_L3_LP_MMZM",
       .field_name = "merged_ozone_concentration",
       .wrong_dims = {1, 2},
       .expected_errno = HARP_ERROR_INGESTION},
      {.module_name = "ESACCI_OZONE_L3_LP_MSMM",
       .field_name = "merged_ozone_concentration",
       .wrong_dims = {1, 2},
       .expected_errno = HARP_ERROR_INGESTION},
  };

  for (const auto& tc : kMissingCases) {
    const harp_ingestion_module* mod = FindIngestionModule(tc.module_name);
    ASSERT_NE(nullptr, mod);

    // 1. Missing dataset.
    {
      CodaMemProduct prod;
      ASSERT_TRUE(prod.ok());
      IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
      EXPECT_EQ(-1, res.status);
      EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
      harp_set_error(HARP_SUCCESS, nullptr);
    }

    // 2. Scalar instead of array.
    {
      CodaMemProduct prod;
      ASSERT_TRUE(prod.ok());
      ASSERT_TRUE(
          prod.AddField(prod.root(), tc.field_name, prod.MakeDouble(1.0)));
      IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
      EXPECT_EQ(-1, res.status);
      EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
      harp_set_error(HARP_SUCCESS, nullptr);
    }

    // 3. Wrong dimensionality.
    {
      CodaMemProduct prod;
      ASSERT_TRUE(prod.ok());
      if (tc.module_name == "ESACCI_OZONE_L3_LP_MMZM") {
        constexpr long kDims3[] = {1, 1, 2};
        constexpr double kVals[] = {1.0, 2.0};
        ASSERT_TRUE(prod.AddField(prod.root(), tc.field_name,
                                  prod.MakeDoubleArray(kDims3, kVals)));
      } else {
        constexpr double kVals[] = {1.0, 2.0};
        ASSERT_TRUE(prod.AddField(prod.root(), tc.field_name,
                                  prod.MakeDoubleArray(tc.wrong_dims, kVals)));
      }
      IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
      EXPECT_EQ(-1, res.status);
      EXPECT_EQ(tc.expected_errno, res.failed_errno);
      harp_set_error(HARP_SUCCESS, nullptr);
    }
  }
}

TEST(HarpIngestCciL3O3LpTest, MmzmDatetimeAndDatasetErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_OZONE_L3_LP_MMZM");
  ASSERT_NE(nullptr, mod);

  constexpr long kDataDims[] = {1, 1};
  constexpr double kConc[] = {10.0};

  // 1. Missing /@year.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "merged_ozone_concentration",
                              prod.MakeDoubleArray(kDataDims, kConc)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 2. Non-string /@year.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "merged_ozone_concentration",
                              prod.MakeDoubleArray(kDataDims, kConc)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "year", prod.MakeInt32(2020)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 3. Wrong length /@year.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "merged_ozone_concentration",
                              prod.MakeDoubleArray(kDataDims, kConc)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "year", prod.MakeString("202")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 4. Missing /@month.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "merged_ozone_concentration",
                              prod.MakeDoubleArray(kDataDims, kConc)));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "year", prod.MakeString("2020")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 5. Non-string /@month.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "merged_ozone_concentration",
                              prod.MakeDoubleArray(kDataDims, kConc)));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "year", prod.MakeString("2020")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "month", prod.MakeInt32(5)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 6. Wrong length /@month.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "merged_ozone_concentration",
                              prod.MakeDoubleArray(kDataDims, kConc)));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "year", prod.MakeString("2020")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "month", prod.MakeString("5")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 7. Invalid date string ("abcd9901") + missing/bad/mismatched datasets.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "merged_ozone_concentration",
                              prod.MakeDoubleArray(kDataDims, kConc)));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "year", prod.MakeString("abcd")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "month", prod.MakeString("99")));
    constexpr long kBadLatDims[] = {2};
    constexpr double kBadLats[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude_centers",
                              prod.MakeDoubleArray(kBadLatDims, kBadLats)));
    constexpr long kVertDims[] = {1};
    constexpr std::string_view kBadAlt[] = {"bad"};
    ASSERT_TRUE(prod.AddField(prod.root(), "approximate_altitude",
                              prod.MakeStringArray(kVertDims, kBadAlt)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["altitude"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_volume_mixing_ratio_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density_uncertainty"].read_status);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // 8. read_as_uncertainty fails when /uncertainty_of_merged_ozone is missing.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "merged_ozone_concentration",
                              prod.MakeDoubleArray(kDataDims, kConc)));
    ASSERT_TRUE(prod.AddField(prod.root(), "merged_ozone_vmr",
                              prod.MakeDoubleArray(kDataDims, kConc)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1,
              res.variables["O3_volume_mixing_ratio_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density_uncertainty"].read_status);
    harp_set_error(HARP_SUCCESS, nullptr);
  }
}

TEST(HarpIngestCciL3O3LpTest, MsmmUncertaintyMissingSecondDataset) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("ESACCI_OZONE_L3_LP_MSMM");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  constexpr long kDataDims[] = {1, 1, 1, 1};
  constexpr double kVal[] = {1.0};
  ASSERT_TRUE(prod.AddField(prod.root(), "merged_ozone_concentration",
                            prod.MakeDoubleArray(kDataDims, kVal)));
  IngestExerciseResult res = ExerciseIngestionModule(
      mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
  EXPECT_EQ(-1, res.status);
  EXPECT_EQ(-1,
            res.variables["O3_volume_mixing_ratio_uncertainty"].read_status);
  EXPECT_EQ(-1, res.variables["O3_number_density_uncertainty"].read_status);
  harp_set_error(HARP_SUCCESS, nullptr);
}

}  // namespace
}  // namespace stcorp_harp
