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
// Unit tests for AFGL86 climatology auxiliary profiles
// (libharp/harp-aux-afgl86.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-aux-afgl86.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-internal.h

#include <array>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::HasSubstr;

constexpr double kSecondsPerTropicalYear = 365.2422 * 86400.0;
constexpr double kJanDatetime = 0.0;                             // frac ~ 0.0
constexpr double kJulyDatetime = 0.5 * kSecondsPerTropicalYear;  // frac ~ 0.5
constexpr double kNovDatetime = 0.85 * kSecondsPerTropicalYear;  // frac ~ 0.85

constexpr auto kAllProfileNames = std::to_array<std::string_view>({
    "altitude",
    "pressure",
    "temperature",
    "number_density",
    "CH4_number_density",
    "CO_number_density",
    "CO2_number_density",
    "H2O_number_density",
    "N2O_number_density",
    "NO2_number_density",
    "O2_number_density",
    "O3_number_density",
});

struct BandScenario {
  std::string_view label;
  double datetime;
  double latitude;
  double expected_surface_temp;
  double expected_surface_air_nd;
};

constexpr auto kBandScenarios = std::to_array<BandScenario>({
    // |latitude| < 30 -> tropic (season-independent).
    {.label = "tropic",
     .datetime = kJanDatetime,
     .latitude = 0.0,
     .expected_surface_temp = 299.700,
     .expected_surface_air_nd = 2.448130E+19},
    // 30 <= |latitude| <= 80 -> midlat.
    {.label = "midlat_north_summer",
     .datetime = kJulyDatetime,
     .latitude = 45.0,
     .expected_surface_temp = 294.200,
     .expected_surface_air_nd = 2.493898E+19},
    {.label = "midlat_north_winter",
     .datetime = kJanDatetime,
     .latitude = 45.0,
     .expected_surface_temp = 272.200,
     .expected_surface_air_nd = 2.708766E+19},
    {.label = "midlat_south_summer_early",
     .datetime = kJanDatetime,
     .latitude = -45.0,
     .expected_surface_temp = 294.200,
     .expected_surface_air_nd = 2.493898E+19},
    {.label = "midlat_south_summer_late",
     .datetime = kNovDatetime,
     .latitude = -45.0,
     .expected_surface_temp = 294.200,
     .expected_surface_air_nd = 2.493898E+19},
    {.label = "midlat_south_winter",
     .datetime = kJulyDatetime,
     .latitude = -45.0,
     .expected_surface_temp = 272.200,
     .expected_surface_air_nd = 2.708766E+19},
    // |latitude| > 80 -> subarctic.
    {.label = "subarctic_north_summer",
     .datetime = kJulyDatetime,
     .latitude = 85.0,
     .expected_surface_temp = 281.700,
     .expected_surface_air_nd = 2.547116E+19},
    {.label = "subarctic_north_winter",
     .datetime = kJanDatetime,
     .latitude = 85.0,
     .expected_surface_temp = 257.200,
     .expected_surface_air_nd = 2.852662E+19},
    {.label = "subarctic_south_summer",
     .datetime = kJanDatetime,
     .latitude = -85.0,
     .expected_surface_temp = 281.700,
     .expected_surface_air_nd = 2.547116E+19},
    {.label = "subarctic_south_winter",
     .datetime = kJulyDatetime,
     .latitude = -85.0,
     .expected_surface_temp = 257.200,
     .expected_surface_air_nd = 2.852662E+19},
});

TEST(HarpAuxAfgl86Test, AllProfilesAcrossAllBandsAndSeasons) {
  for (const auto& scenario : kBandScenarios) {
    SCOPED_TRACE(std::string(scenario.label));
    for (const std::string_view name_sv : kAllProfileNames) {
      const std::string name(name_sv);
      int num_vertical = -1;
      const double* values = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_aux_afgl86_get_profile(
                                  name.c_str(), scenario.datetime,
                                  scenario.latitude, &num_vertical, &values))
          << "profile=" << name;
      EXPECT_EQ(50, num_vertical);
      ASSERT_NE(nullptr, values);
      // Verify monotonic positive values across all 50 levels (altitude starts
      // at 0.0 and increases to 120000.0).
      if (name == "altitude") {
        EXPECT_DOUBLE_EQ(0.0, values[0]);
        EXPECT_DOUBLE_EQ(120000.0, values[49]);
      } else {
        EXPECT_GT(values[0], 0.0);
        EXPECT_GT(values[49], 0.0);
      }
      if (name == "temperature") {
        // Only check exact surface temperature for scenarios where we pinned it
        // directly (tropic/midlat); subarctic temperatures are > 200 K.
        if (scenario.label == "tropic" ||
            scenario.label.starts_with("midlat")) {
          EXPECT_DOUBLE_EQ(scenario.expected_surface_temp, values[0]);
        } else {
          EXPECT_GT(values[0], 200.0);
        }
      }
      if (name == "number_density") {
        EXPECT_DOUBLE_EQ(scenario.expected_surface_air_nd, values[0]);
      }
    }
  }
}

TEST(HarpAuxAfgl86Test, LatitudeAndSeasonBoundaryConditions) {
  int num_vertical = -1;
  const double* values_boundary = nullptr;
  const double* values_tropic = nullptr;

  // Latitude == 30.0 is NOT < 30.0, so it selects midlat rather than tropic.
  ASSERT_EQ(HARP_SUCCESS,
            harp_aux_afgl86_get_profile("temperature", kJulyDatetime, 30.0,
                                        &num_vertical, &values_boundary));
  ASSERT_EQ(HARP_SUCCESS,
            harp_aux_afgl86_get_profile("temperature", kJulyDatetime, 29.9,
                                        &num_vertical, &values_tropic));
  EXPECT_DOUBLE_EQ(294.200, values_boundary[0]);  // midlat_summer
  EXPECT_DOUBLE_EQ(299.700, values_tropic[0]);    // tropic

  // Latitude == -30.0 is NOT > -30.0, so it selects midlat.
  ASSERT_EQ(HARP_SUCCESS,
            harp_aux_afgl86_get_profile("temperature", kJanDatetime, -30.0,
                                        &num_vertical, &values_boundary));
  EXPECT_DOUBLE_EQ(294.200, values_boundary[0]);  // midlat_summer (SH Jan)

  // Latitude == 80.0 and -80.0 are NOT > 80 / < -80, so they select midlat.
  ASSERT_EQ(HARP_SUCCESS,
            harp_aux_afgl86_get_profile("number_density", kJanDatetime, 80.0,
                                        &num_vertical, &values_boundary));
  EXPECT_DOUBLE_EQ(2.708766E+19, values_boundary[0]);  // midlat_winter
  ASSERT_EQ(HARP_SUCCESS,
            harp_aux_afgl86_get_profile("number_density", kJanDatetime, -80.0,
                                        &num_vertical, &values_boundary));
  EXPECT_DOUBLE_EQ(2.493898E+19, values_boundary[0]);  // midlat_summer

  // Exact fraction_of_year == 0.25 and 0.75 boundaries in Northern hemisphere.
  ASSERT_EQ(HARP_SUCCESS, harp_aux_afgl86_get_profile(
                              "temperature", 0.25 * kSecondsPerTropicalYear,
                              45.0, &num_vertical, &values_boundary));
  EXPECT_DOUBLE_EQ(294.200, values_boundary[0]);  // summer (>= 0.25)

  ASSERT_EQ(HARP_SUCCESS, harp_aux_afgl86_get_profile(
                              "temperature", 0.75 * kSecondsPerTropicalYear,
                              45.0, &num_vertical, &values_boundary));
  EXPECT_DOUBLE_EQ(272.200, values_boundary[0]);  // winter (>= 0.75)

  // Northern hemisphere late-year (>= 0.75) selects winter.
  ASSERT_EQ(HARP_SUCCESS,
            harp_aux_afgl86_get_profile("temperature", kNovDatetime, 45.0,
                                        &num_vertical, &values_boundary));
  EXPECT_DOUBLE_EQ(272.200, values_boundary[0]);
}

TEST(HarpAuxAfgl86Test, UnknownProfileNameReturnsInvalidArgument) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  int num_vertical = 123;
  const double* values = nullptr;
  EXPECT_EQ(-1, harp_aux_afgl86_get_profile("nonexistent_species", kJanDatetime,
                                            0.0, &num_vertical, &values));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_THAT(harp_errno_to_string(HARP_ERROR_INVALID_ARGUMENT),
              HasSubstr("AFGL86 climatology does not have a profile for "
                        "'nonexistent_species'"));
  // Output parameters are not overwritten on error.
  EXPECT_EQ(123, num_vertical);
  EXPECT_EQ(nullptr, values);
}

}  // namespace
}  // namespace stcorp_harp
