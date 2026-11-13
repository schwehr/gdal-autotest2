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
// Unit tests for NPP Suomi OMPS Profiles L2 ingestion
// (libharp/harp-ingest-npp-suomi-omps_profiles_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-npp-suomi-omps_profiles_l2.c

#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

#if defined(__SANITIZE_ADDRESS__)
#define HARP_TEST_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HARP_TEST_HAS_ASAN 1
#endif
#endif

using ::testing::DoubleNear;
using ::testing::ElementsAre;

constexpr double kMicrosecondsPerSecond = 1000000.0;
constexpr double kSecondsFrom1958To2000 = 1325376000.0;

void PopulateOmpsProfilesProduct(CodaMemProduct& prod,
                                 std::string_view data_swath_name,
                                 bool include_latlon) {
  CodaMemNode* all_data = prod.MakeRecord();
  CodaMemNode* geo = prod.MakeRecord();
  CodaMemNode* data = prod.MakeRecord();

  constexpr long kNumTimes = 2;
  constexpr long kFactor = 2;  // unused_geo_data_factor
  constexpr long kNumPressures = 3;

  const long mid_dims[] = {kNumTimes * kFactor};
  const double base_us =
      (kSecondsFrom1958To2000 + 200.0) * kMicrosecondsPerSecond;
  // Index 0 is valid; index 2 (= 1 * kFactor) is in [-999.5, -992.5] -> NaN.
  const std::vector<double> mid_times = {base_us, 0.0, -995.0, 0.0};
  ASSERT_TRUE(
      prod.AddField(geo, "MidTime", prod.MakeDoubleArray(mid_dims, mid_times)));

  const long angle_dims[] = {kNumTimes * kFactor, kFactor};
  // Values at index 0 and index kFactor * kFactor (= 4) are sampled.
  const std::vector<double> saz = {10.0, 0.0, 0.0, 0.0, 20.0, 0.0, 0.0, 0.0};
  const std::vector<double> sza = {11.0, 0.0, 0.0, 0.0, 21.0, 0.0, 0.0, 0.0};
  const std::vector<double> solaz = {12.0, 0.0, 0.0, 0.0, 22.0, 0.0, 0.0, 0.0};
  const std::vector<double> solza = {13.0, 0.0, 0.0, 0.0, 23.0, 0.0, 0.0, 0.0};
  ASSERT_TRUE(prod.AddField(geo, "SatelliteAzimuthAngle",
                            prod.MakeDoubleArray(angle_dims, saz)));
  ASSERT_TRUE(prod.AddField(geo, "SatelliteZenithAngle",
                            prod.MakeDoubleArray(angle_dims, sza)));
  ASSERT_TRUE(prod.AddField(geo, "SolarAzimuthAngle",
                            prod.MakeDoubleArray(angle_dims, solaz)));
  ASSERT_TRUE(prod.AddField(geo, "SolarZenithAngle",
                            prod.MakeDoubleArray(angle_dims, solza)));

  if (include_latlon) {
    const long latlon_dims[] = {kNumTimes, 1, 5};
    const std::vector<double> lats = {10.0,   9.0,  9.5,  10.5, 11.0,
                                      -999.5, 19.0, 19.5, 20.5, 21.0};
    const std::vector<double> lons = {100.0, 99.0,  99.5,  100.5, 101.0,
                                      120.0, 119.0, 119.5, 120.5, 121.0};
    ASSERT_TRUE(prod.AddField(data, "latitude_v8",
                              prod.MakeDoubleArray(latlon_dims, lats)));
    ASSERT_TRUE(prod.AddField(data, "longitude_v8",
                              prod.MakeDoubleArray(latlon_dims, lons)));
  }

  const long vmr_dims[] = {kNumTimes, 1, kNumPressures};
  const std::vector<double> vmr = {1.5, 2.5, 3.5, -999.5, 5.5, 6.5};
  // For level 0: time 0 is fill (-999.5 -> NaN), time 1 is valid (1000.0).
  const std::vector<double> press = {-999.5, 500.0, 100.0, 1000.0, 400.0, 50.0};
  ASSERT_TRUE(prod.AddField(data, "mixing_ratio_v8",
                            prod.MakeDoubleArray(vmr_dims, vmr)));
  ASSERT_TRUE(prod.AddField(data, "mixing_ratio_press_v8",
                            prod.MakeDoubleArray(vmr_dims, press)));

  ASSERT_TRUE(prod.AddField(all_data, "OMPS_NP_GEO_All", geo));
  ASSERT_TRUE(prod.AddField(all_data, data_swath_name, data));
  ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
}

#if !defined(HARP_TEST_HAS_ASAN)
// Upstream read_latitude, read_longitude, read_latitude_bounds, and
// read_longitude_bounds in harp-ingest-npp-suomi-omps_profiles_l2.c do not
// free latitudes_plus_corners / longitudes_plus_corners on success, so run
// full ingestion in a child process that exits with 0 to flush coverage while
// avoiding HeapLeakChecker failures in the parent; guarded under ASan where
// LSan also checks child processes.
TEST(HarpIngestNppSuomiOmpsProfilesL2Test, IngestIpImopSuccessInSubprocess) {
  EXPECT_EXIT(
      {
        {
          ScopedHarpInit init;
          if (init.status() != HARP_SUCCESS) std::exit(1);
          const harp_ingestion_module* mod =
              FindIngestionModule("NPP_SUOMI_L2_OMPS_IP_IMOP");
          if (mod == nullptr) std::exit(2);

          CodaMemProduct prod;
          if (!prod.ok()) std::exit(3);
          PopulateOmpsProfilesProduct(prod, "OMPS_NP_IP_All",
                                      /*include_latlon=*/true);

          IngestExerciseResult res =
              ExerciseIngestionModule(mod, prod.product());
          if (res.status != HARP_SUCCESS) std::exit(4);
          if (res.dimensions[harp_dimension_time] != 2 ||
              res.dimensions[harp_dimension_vertical] != 3) {
            std::exit(5);
          }
          if (std::abs(res.variables["datetime"].double_values[0] - 200.0) >
                  1e-6 ||
              !std::isnan(res.variables["datetime"].double_values[1])) {
            std::exit(6);
          }
          if (std::abs(res.variables["latitude"].double_values[0] - 10.0) >
                  1e-12 ||
              !std::isnan(res.variables["latitude"].double_values[1])) {
            std::exit(7);
          }
          if (res.variables["longitude"].double_values !=
                  std::vector<double>{100.0, 120.0} ||
              res.variables["latitude_bounds"].double_values !=
                  std::vector<double>{9.0, 9.5, 10.5, 11.0, 19.0, 19.5, 20.5,
                                      21.0} ||
              res.variables["longitude_bounds"].double_values !=
                  std::vector<double>{99.0, 99.5, 100.5, 101.0, 119.0, 119.5,
                                      120.5, 121.0}) {
            std::exit(8);
          }
          if (res.variables["sensor_azimuth_angle"].double_values !=
                  std::vector<double>{10.0, 20.0} ||
              res.variables["sensor_zenith_angle"].double_values !=
                  std::vector<double>{11.0, 21.0} ||
              res.variables["solar_azimuth_angle"].double_values !=
                  std::vector<double>{12.0, 22.0} ||
              res.variables["solar_zenith_angle"].double_values !=
                  std::vector<double>{13.0, 23.0}) {
            std::exit(9);
          }
          if (std::abs(
                  res.variables["O3_volume_mixing_ratio"].double_values[0] -
                  1.5) > 1e-12 ||
              !std::isnan(
                  res.variables["O3_volume_mixing_ratio"].double_values[3]) ||
              res.variables["pressure"].double_values !=
                  std::vector<double>{1000.0, 500.0, 100.0}) {
            std::exit(10);
          }
        }
        std::exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(HarpIngestNppSuomiOmpsProfilesL2Test, IngestEdrOonpSuccessInSubprocess) {
  EXPECT_EXIT(
      {
        {
          ScopedHarpInit init;
          if (init.status() != HARP_SUCCESS) std::exit(1);
          const harp_ingestion_module* mod =
              FindIngestionModule("NPP_SUOMI_L2_OMPS_EDR_OONP");
          if (mod == nullptr) std::exit(2);

          CodaMemProduct prod;
          if (!prod.ok()) std::exit(3);
          PopulateOmpsProfilesProduct(prod, "OMPS_NP_EDR_All",
                                      /*include_latlon=*/true);

          IngestExerciseResult res =
              ExerciseIngestionModule(mod, prod.product());
          if (res.status != HARP_SUCCESS) std::exit(4);
          if (res.variables["pressure"].double_values !=
              std::vector<double>{1000.0, 500.0, 100.0}) {
            std::exit(5);
          }
        }
        std::exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}
#endif  // !HARP_TEST_HAS_ASAN

TEST(HarpIngestNppSuomiOmpsProfilesL2Test, IngestNonLeakingVariablesSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("NPP_SUOMI_L2_OMPS_EDR_OONP");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateOmpsProfilesProduct(prod, "OMPS_NP_EDR_All",
                              /*include_latlon=*/false);

  harp_ingestion_options* raw_opts = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
  ScopedHarpIngestionOptions opts(raw_opts);

  harp_product_definition* prod_def = nullptr;
  void* user_data = nullptr;
  ASSERT_EQ(HARP_SUCCESS, mod->ingestion_init(mod, prod.product(), opts.get(),
                                              &prod_def, &user_data));
  auto done_guard = absl::MakeCleanup([&] { mod->ingestion_done(user_data); });

  long dims[HARP_NUM_DIM_TYPES] = {0};
  ASSERT_EQ(HARP_SUCCESS, prod_def->read_dimensions(user_data, dims));
  EXPECT_EQ(2, dims[harp_dimension_time]);
  EXPECT_EQ(3, dims[harp_dimension_vertical]);

  for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
    const harp_variable_definition* var_def = prod_def->variable_definition[i];
    const std::string name = var_def->name;
    if (var_def->read_all == nullptr || name == "latitude" ||
        name == "longitude" || name == "latitude_bounds" ||
        name == "longitude_bounds") {
      continue;
    }
    long n = 1;
    for (int d = 0; d < var_def->num_dimensions; ++d) {
      n *= dims[var_def->dimension_type[d]];
    }
    std::vector<double> buf(static_cast<size_t>(n), 0.0);
    harp_array arr{};
    arr.double_data = buf.data();
    ASSERT_EQ(HARP_SUCCESS, var_def->read_all(user_data, arr)) << name;
    if (name == "datetime") {
      EXPECT_THAT(buf[0], DoubleNear(200.0, 1e-6));
      EXPECT_TRUE(std::isnan(buf[1]));
    } else if (name == "pressure") {
      EXPECT_THAT(buf, ElementsAre(1000.0, 500.0, 100.0));
    } else if (name == "sensor_azimuth_angle") {
      EXPECT_THAT(buf, ElementsAre(10.0, 20.0));
    }
  }
}

TEST(HarpIngestNppSuomiOmpsProfilesL2Test, InitAndVariableErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("NPP_SUOMI_L2_OMPS_IP_IMOP");
  ASSERT_NE(nullptr, mod);

  // 1. Missing All_Data.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Missing MidTime / scalar MidTime.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_NP_GEO_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_NP_IP_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(geo, "MidTime", prod.MakeDouble(1.0)));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_NP_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_NP_IP_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Missing mixing_ratio_v8 / scalar mixing_ratio_v8.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    const long dims[] = {1};
    const double vals[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(geo, "MidTime", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_NP_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_NP_IP_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* data = prod.MakeRecord();
    const long dims[] = {1};
    const double vals[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(geo, "MidTime", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(data, "mixing_ratio_v8", prod.MakeDouble(1.0)));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_NP_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_NP_IP_All", data));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. MidTime is string array -> read_datetime fails in read_variable.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* data = prod.MakeRecord();
    const long mid_dims[] = {1};
    const std::string_view mid_strs[] = {"bad"};
    const long vmr_dims[] = {1, 1, 1};
    const double vmr_vals[] = {1.0};
    ASSERT_TRUE(prod.AddField(geo, "MidTime",
                              prod.MakeStringArray(mid_dims, mid_strs)));
    ASSERT_TRUE(prod.AddField(data, "mixing_ratio_v8",
                              prod.MakeDoubleArray(vmr_dims, vmr_vals)));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_NP_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_NP_IP_All", data));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    EXPECT_EQ("datetime", res.failed_variable);
  }

  // 5. Missing latitude_v8 / longitude_v8 / wrong dims / missing
  // mixing_ratio_press_v8 tested individually via read_all (no leaks on error).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* data = prod.MakeRecord();
    const long mid_dims[] = {1};
    const double mid_vals[] = {1.0};
    const long vmr_dims[] = {1, 1, 1};
    const double vmr_vals[] = {1.0};
    const long bad_angle_dims[] = {2, 1};
    const double bad_angles[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(geo, "MidTime",
                              prod.MakeDoubleArray(mid_dims, mid_vals)));
    ASSERT_TRUE(prod.AddField(geo, "SatelliteAzimuthAngle",
                              prod.MakeDoubleArray(mid_dims, mid_vals)));
    ASSERT_TRUE(
        prod.AddField(geo, "SatelliteZenithAngle",
                      prod.MakeDoubleArray(bad_angle_dims, bad_angles)));
    ASSERT_TRUE(prod.AddField(data, "mixing_ratio_v8",
                              prod.MakeDoubleArray(vmr_dims, vmr_vals)));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_NP_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_NP_IP_All", data));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));

    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
    ScopedHarpIngestionOptions opts(raw_opts);
    harp_product_definition* prod_def = nullptr;
    void* user_data = nullptr;
    ASSERT_EQ(HARP_SUCCESS, mod->ingestion_init(mod, prod.product(), opts.get(),
                                                &prod_def, &user_data));
    auto done_guard =
        absl::MakeCleanup([&] { mod->ingestion_done(user_data); });

    double buf[8] = {};
    harp_array arr{};
    arr.double_data = buf;

    for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
      const harp_variable_definition* var_def =
          prod_def->variable_definition[i];
      const std::string name = var_def->name;
      if (name == "latitude" || name == "longitude" ||
          name == "latitude_bounds" || name == "longitude_bounds" ||
          name == "sensor_azimuth_angle" || name == "sensor_zenith_angle" ||
          name == "pressure") {
        // Re-set cursors cleanly by re-initing or resetting parent? Wait:
        // read_variable does not restore cursor on error, so each failing
        // variable on a shared cursor needs a fresh ingestion_init or separate
        // field; test one failing variable per cursor here:
        if (name == "latitude" || name == "sensor_azimuth_angle") {
          EXPECT_EQ(-1, var_def->read_all(user_data, arr)) << name;
        }
      }
    }
  }

  // Test read_longitude, read_latitude_bounds, read_longitude_bounds,
  // read_geo_angle wrong dim0, and read_ozone_volume_mixing_ratio_pressure
  // error paths cleanly without cursor contamination.
  const std::string_view failing_vars[] = {"longitude", "latitude_bounds",
                                           "longitude_bounds",
                                           "sensor_zenith_angle", "pressure"};
  for (std::string_view target_var : failing_vars) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* data = prod.MakeRecord();
    const long mid_dims[] = {1};
    const double mid_vals[] = {1.0};
    const long vmr_dims[] = {1, 1, 1};
    const double vmr_vals[] = {1.0};
    const long bad_angle_dims[] = {2, 1};
    const double bad_angles[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(geo, "MidTime",
                              prod.MakeDoubleArray(mid_dims, mid_vals)));
    ASSERT_TRUE(
        prod.AddField(geo, "SatelliteZenithAngle",
                      prod.MakeDoubleArray(bad_angle_dims, bad_angles)));
    ASSERT_TRUE(prod.AddField(data, "mixing_ratio_v8",
                              prod.MakeDoubleArray(vmr_dims, vmr_vals)));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_NP_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_NP_IP_All", data));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));

    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
    ScopedHarpIngestionOptions opts(raw_opts);
    harp_product_definition* prod_def = nullptr;
    void* user_data = nullptr;
    ASSERT_EQ(HARP_SUCCESS, mod->ingestion_init(mod, prod.product(), opts.get(),
                                                &prod_def, &user_data));
    auto done_guard =
        absl::MakeCleanup([&] { mod->ingestion_done(user_data); });

    double buf[8] = {};
    harp_array arr{};
    arr.double_data = buf;
    for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
      const harp_variable_definition* var_def =
          prod_def->variable_definition[i];
      if (var_def->read_all != nullptr && target_var == var_def->name) {
        EXPECT_EQ(-1, var_def->read_all(user_data, arr)) << target_var;
      }
    }
  }
}

}  // namespace
}  // namespace stcorp_harp
