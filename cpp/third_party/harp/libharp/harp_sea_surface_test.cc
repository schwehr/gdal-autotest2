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
// Unit tests for ocean wave dispersion and sea surface temperature
// conversion functions (libharp/harp-sea-surface.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-sea-surface.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-internal.h

#include <array>
#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kGravity = CONST_GRAV_ACCEL;

TEST(HarpSeaSurfaceTest, OceanFrequencyAndPeriodReciprocals) {
  constexpr double kPeriod = 8.0;
  constexpr double kFreq = 0.125;
  EXPECT_DOUBLE_EQ(kFreq, harp_ocean_frequency_from_ocean_period(kPeriod));
  EXPECT_DOUBLE_EQ(kPeriod, harp_ocean_period_from_ocean_frequency(kFreq));

  // Round-trip for arbitrary positive values.
  constexpr double kTestPeriod = 12.5;
  EXPECT_NEAR(kTestPeriod,
              harp_ocean_period_from_ocean_frequency(
                  harp_ocean_frequency_from_ocean_period(kTestPeriod)),
              1e-14);
}

TEST(HarpSeaSurfaceTest, OceanWavelengthAndWavenumberReciprocals) {
  constexpr double kWavelength = 100.0;
  const double expected_wavenumber = 2.0 * kPi / kWavelength;
  EXPECT_DOUBLE_EQ(expected_wavenumber,
                   harp_ocean_wavenumber_from_ocean_wavelength(kWavelength));
  EXPECT_DOUBLE_EQ(kWavelength, harp_ocean_wavelength_from_ocean_wavenumber(
                                    expected_wavenumber));
}

TEST(HarpSeaSurfaceTest, DeepWaterDispersionConversions) {
  constexpr double kFrequency = 0.25;
  constexpr double kPeriod = 4.0;
  constexpr double kWavelength = 156.0;
  constexpr double kWavenumber = 0.04;

  // Wavelength <-> frequency and period.
  EXPECT_DOUBLE_EQ(kGravity / (2.0 * kPi * kFrequency * kFrequency),
                   harp_ocean_wavelength_from_ocean_frequency(kFrequency));
  EXPECT_DOUBLE_EQ(kPeriod * kPeriod * kGravity / (2.0 * kPi),
                   harp_ocean_wavelength_from_ocean_period(kPeriod));

  // Wavenumber <-> frequency and period.
  EXPECT_DOUBLE_EQ(4.0 * kPi * kPi * kFrequency * kFrequency / kGravity,
                   harp_ocean_wavenumber_from_ocean_frequency(kFrequency));
  EXPECT_DOUBLE_EQ(4.0 * kPi * kPi / (kPeriod * kPeriod * kGravity),
                   harp_ocean_wavenumber_from_ocean_period(kPeriod));
  EXPECT_DOUBLE_EQ(std::sqrt(kWavenumber * kGravity) / (2.0 * kPi),
                   harp_ocean_frequency_from_ocean_wavenumber(kWavenumber));

  // Pin upstream formulas in harp-sea-surface.c for frequency/period from
  // wavelength and period from wavenumber.
  EXPECT_DOUBLE_EQ(std::sqrt(2.0 * kPi * kWavelength / kGravity),
                   harp_ocean_frequency_from_ocean_wavelength(kWavelength));
  EXPECT_DOUBLE_EQ(std::sqrt(kGravity / (2.0 * kPi * kWavelength)),
                   harp_ocean_period_from_ocean_wavelength(kWavelength));
  EXPECT_DOUBLE_EQ(std::sqrt(kWavenumber * kGravity) / (2.0 * kPi),
                   harp_ocean_period_from_ocean_wavenumber(kWavenumber));
}

TEST(HarpSeaSurfaceTest, SeaSurfaceTemperatureSkinAndSubskinConversions) {
  struct SstCase {
    double sst_subskin;
    double wind_speed;
    double solar_zenith_angle;
    double effective_wind_speed;
  };
  // Daytime (SZA <= 90.0) clamps wind_speed < 6.0 m/s to 6.0 m/s.
  // Nighttime (SZA > 90.0) and wind_speed >= 6.0 m/s use wind_speed directly.
  constexpr auto kCases = std::to_array<SstCase>({
      {.sst_subskin = 290.0,
       .wind_speed = 2.0,
       .solar_zenith_angle = 45.0,
       .effective_wind_speed = 6.0},
      {.sst_subskin = 295.0,
       .wind_speed = 3.5,
       .solar_zenith_angle = 90.0,
       .effective_wind_speed = 6.0},
      {.sst_subskin = 285.0,
       .wind_speed = 6.0,
       .solar_zenith_angle = 30.0,
       .effective_wind_speed = 6.0},
      {.sst_subskin = 288.0,
       .wind_speed = 10.0,
       .solar_zenith_angle = 20.0,
       .effective_wind_speed = 10.0},
      {.sst_subskin = 280.0,
       .wind_speed = 2.0,
       .solar_zenith_angle = 95.0,
       .effective_wind_speed = 2.0},
  });

  for (const auto& tc : kCases) {
    const double delta = 0.14 + 0.30 * std::exp(-tc.effective_wind_speed / 3.7);
    const double expected_skin = tc.sst_subskin - delta;
    const double actual_skin =
        harp_sea_surface_temperature_skin_from_subskin_wind_speed_and_solar_zenith_angle(
            tc.sst_subskin, tc.wind_speed, tc.solar_zenith_angle);
    EXPECT_DOUBLE_EQ(expected_skin, actual_skin);

    const double actual_subskin =
        harp_sea_surface_temperature_subskin_from_skin_wind_speed_and_solar_zenith_angle(
            actual_skin, tc.wind_speed, tc.solar_zenith_angle);
    EXPECT_NEAR(tc.sst_subskin, actual_subskin, 1e-12);
  }
}

}  // namespace
}  // namespace stcorp_harp
