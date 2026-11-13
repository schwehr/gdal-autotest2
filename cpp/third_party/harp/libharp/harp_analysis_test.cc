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
// Unit tests for libharp scientific analysis helper functions
// (libharp/harp-analysis.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-analysis.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-internal.h

#include <array>
#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

TEST(HarpAnalysisTest, AngstromExponentFromAod) {
  constexpr auto kSingleWavelength = std::to_array<double>({500e-9});
  constexpr auto kSingleAod = std::to_array<double>({0.25});
  EXPECT_EQ(1, harp_isnan(harp_angstrom_exponent_from_aod(
                   0, kSingleWavelength.data(), kSingleAod.data())));
  EXPECT_EQ(1, harp_isnan(harp_angstrom_exponent_from_aod(
                   1, kSingleWavelength.data(), kSingleAod.data())));

  // Exact power law tau(lambda) = tau0 * (lambda / lambda0)^(-alpha) with
  // alpha = 1.5 across three wavelengths.
  constexpr auto kWavelengths = std::to_array<double>({400e-9, 550e-9, 800e-9});
  const auto kAods = std::to_array<double>({
      0.5 * std::pow(400.0 / 550.0, -1.5),
      0.5,
      0.5 * std::pow(800.0 / 550.0, -1.5),
  });
  EXPECT_NEAR(1.5,
              harp_angstrom_exponent_from_aod(
                  kWavelengths.size(), kWavelengths.data(), kAods.data()),
              1e-12);
}

TEST(HarpAnalysisTest, FractionOfDayAndYearFromDatetime) {
  struct FractionCase {
    double datetime;
    double expected;
  };
  constexpr FractionCase kDayCases[] = {
      {.datetime = 0.0, .expected = 0.0},
      {.datetime = 21600.0, .expected = 0.25},
      {.datetime = 86400.0 * 3.75, .expected = 0.75},
      {.datetime = -21600.0, .expected = 0.75},
  };
  for (const auto& tc : kDayCases) {
    EXPECT_NEAR(tc.expected, harp_fraction_of_day_from_datetime(tc.datetime),
                1e-12);
  }

  constexpr double kSecondsPerTropicalYear = 365.2422 * 86400.0;
  constexpr FractionCase kYearCases[] = {
      {.datetime = 0.0, .expected = 0.0},
      {.datetime = 0.5 * kSecondsPerTropicalYear, .expected = 0.5},
      {.datetime = -0.25 * kSecondsPerTropicalYear, .expected = 0.75},
  };
  for (const auto& tc : kYearCases) {
    EXPECT_NEAR(tc.expected, harp_fraction_of_year_from_datetime(tc.datetime),
                1e-12);
  }
}

TEST(HarpAnalysisTest, WavelengthWavenumberFrequencyConversions) {
  constexpr double kWavelength = 500e-9;
  const double frequency = harp_frequency_from_wavelength(kWavelength);
  const double wavenumber = harp_wavenumber_from_wavelength(kWavelength);

  EXPECT_NEAR(CONST_SPEED_OF_LIGHT / kWavelength, frequency, 1e-6);
  EXPECT_NEAR(1.0 / kWavelength, wavenumber, 1e-6);
  EXPECT_NEAR(frequency, harp_frequency_from_wavenumber(wavenumber), 1e-6);
  EXPECT_NEAR(kWavelength, harp_wavelength_from_frequency(frequency), 1e-18);
  EXPECT_NEAR(kWavelength, harp_wavelength_from_wavenumber(wavenumber), 1e-18);
  EXPECT_NEAR(wavenumber, harp_wavenumber_from_frequency(frequency), 1e-6);
}

TEST(HarpAnalysisTest, GravityAndCurvatureRadius) {
  const double g_eq = harp_normal_gravity_from_latitude(0.0);
  const double g_pole = harp_normal_gravity_from_latitude(90.0);
  EXPECT_NEAR(9.7803253359, g_eq, 1e-10);
  EXPECT_NEAR(9.8321849378, g_pole, 1e-7);

  EXPECT_NEAR(g_eq, harp_gravity_from_latitude_and_altitude(0.0, 0.0), 1e-12);
  const double g_alt = harp_gravity_from_latitude_and_altitude(45.0, 10000.0);
  EXPECT_LT(g_alt, harp_normal_gravity_from_latitude(45.0));
  EXPECT_GT(g_alt, 9.7);

  EXPECT_NEAR(6356752.0,
              harp_local_curvature_radius_at_surface_from_latitude(0.0), 1e-6);
  EXPECT_NEAR(6378137.0,
              harp_local_curvature_radius_at_surface_from_latitude(90.0), 1e-6);
  const double r_mid =
      harp_local_curvature_radius_at_surface_from_latitude(45.0);
  EXPECT_GT(r_mid, 6356752.0);
  EXPECT_LT(r_mid, 6378137.0);
}

TEST(HarpAnalysisTest, RadianceReflectanceAndNormalizedRadiance) {
  constexpr double kSolarIrradiance = 1500.0;
  constexpr double kSza = 60.0;  // cos(60 deg) = 0.5
  constexpr double kReflectance = 0.4;

  const double radiance =
      harp_radiance_from_reflectance_solar_irradiance_and_solar_zenith_angle(
          kReflectance, kSolarIrradiance, kSza);
  EXPECT_NEAR(kReflectance * 0.5 * kSolarIrradiance / std::numbers::pi,
              radiance, 1e-9);
  EXPECT_NEAR(
      kReflectance,
      harp_reflectance_from_radiance_solar_irradiance_and_solar_zenith_angle(
          radiance, kSolarIrradiance, kSza),
      1e-12);

  const double norm_rad =
      harp_normalized_radiance_from_radiance_and_solar_irradiance(
          radiance, kSolarIrradiance);
  EXPECT_NEAR(norm_rad,
              harp_normalized_radiance_from_reflectance_and_solar_zenith_angle(
                  kReflectance, kSza),
              1e-12);
  EXPECT_NEAR(radiance,
              harp_radiance_from_normalized_radiance_and_solar_irradiance(
                  norm_rad, kSolarIrradiance),
              1e-12);
  EXPECT_NEAR(kReflectance,
              harp_reflectance_from_normalized_radiance_and_solar_zenith_angle(
                  norm_rad, kSza),
              1e-12);
}

TEST(HarpAnalysisTest, SolarAnglesAndAzimuths) {
  // Backscattering (both at zenith -> 180 deg) and forward scattering (0, 180).
  EXPECT_NEAR(180.0,
              harp_scattering_angle_from_sensor_and_solar_angles(0.0, 0.0, 0.0),
              1e-12);
  EXPECT_NEAR(
      0.0, harp_scattering_angle_from_sensor_and_solar_angles(0.0, 180.0, 0.0),
      1e-12);
  EXPECT_NEAR(
      90.0,
      harp_scattering_angle_from_sensor_and_solar_angles(45.0, 45.0, 180.0),
      1e-11);

  // Solar declination across solstice / equinox timestamps.
  const double dec_jan1 = harp_solar_declination_angle_from_datetime(0.0);
  const double dec_jun21 =
      harp_solar_declination_angle_from_datetime(172.0 * 86400.0);
  EXPECT_LT(dec_jan1, -20.0);
  EXPECT_GT(dec_jun21, 20.0);

  // Solar hour angle covers EOT, positive and negative datetimes.
  const double sha_noon =
      harp_solar_hour_angle_from_datetime_and_longitude(43200.0, 0.0);
  EXPECT_NEAR(0.0, sha_noon, 5.0);
  const double sha_neg =
      harp_solar_hour_angle_from_datetime_and_longitude(-43200.0, 90.0);
  EXPECT_GE(sha_neg, -180.0);
  EXPECT_LT(sha_neg, 180.0);

  // Solar zenith angle at equator, equinox, noon vs horizon.
  EXPECT_NEAR(
      0.0,
      harp_solar_zenith_angle_from_latitude_and_solar_angles(0.0, 0.0, 0.0),
      1e-12);
  EXPECT_NEAR(
      90.0,
      harp_solar_zenith_angle_from_latitude_and_solar_angles(90.0, 0.0, 0.0),
      1e-12);
  EXPECT_NEAR(
      180.0,
      harp_solar_zenith_angle_from_latitude_and_solar_angles(0.0, 0.0, 180.0),
      1e-12);

  // Solar azimuth: sin_sza == 0 -> 0; morning (sha <= 0) -> >= 0; afternoon
  // (sha > 0) -> <= 0.
  EXPECT_DOUBLE_EQ(0.0, harp_solar_azimuth_angle_from_latitude_and_solar_angles(
                            45.0, 10.0, 30.0, 0.0));
  EXPECT_GT(harp_solar_azimuth_angle_from_latitude_and_solar_angles(
                45.0, 0.0, -45.0, 60.0),
            0.0);
  EXPECT_LT(harp_solar_azimuth_angle_from_latitude_and_solar_angles(45.0, 0.0,
                                                                    45.0, 60.0),
            0.0);

  // Relative azimuth covers < -1e4, > 1e4, < 0, >= 360, > 180, and [0, 180].
  EXPECT_NEAR(30.0,
              harp_relative_azimuth_angle_from_sensor_and_solar_azimuth_angles(
                  50.0, 20.0),
              1e-12);
  EXPECT_NEAR(90.0,
              harp_relative_azimuth_angle_from_sensor_and_solar_azimuth_angles(
                  10.0, 100.0),
              1e-12);
  EXPECT_NEAR(90.0,
              harp_relative_azimuth_angle_from_sensor_and_solar_azimuth_angles(
                  300.0, 30.0),
              1e-12);
  EXPECT_NEAR(90.0,
              harp_relative_azimuth_angle_from_sensor_and_solar_azimuth_angles(
                  810.0, 0.0),
              1e-12);
  EXPECT_NEAR(10.0,
              harp_relative_azimuth_angle_from_sensor_and_solar_azimuth_angles(
                  18010.0, 0.0),
              1e-9);
  EXPECT_NEAR(10.0,
              harp_relative_azimuth_angle_from_sensor_and_solar_azimuth_angles(
                  -18010.0, 0.0),
              1e-9);

  // Elevation / zenith and sensor / viewing angle involutions.
  EXPECT_DOUBLE_EQ(30.0, harp_elevation_angle_from_zenith_angle(60.0));
  EXPECT_DOUBLE_EQ(60.0, harp_zenith_angle_from_elevation_angle(30.0));
  EXPECT_DOUBLE_EQ(135.0, harp_sensor_angle_from_viewing_angle(45.0));
  EXPECT_DOUBLE_EQ(45.0, harp_viewing_angle_from_sensor_angle(135.0));
}

TEST(HarpAnalysisTest, SensorGeometryAnglesAndProfiles) {
  double sza = 0.0;
  double vza = 0.0;
  double raa = 0.0;

  // 1. Nadir geometry (vza == 0) and equal-altitude short-circuit, including
  // relative azimuth > 180 wrapping.
  EXPECT_EQ(0, harp_sensor_geometry_angles_at_altitude_from_other_altitude(
                   0.0, 40.0, 0.0, 200.0, 10000.0, &sza, &vza, &raa));
  EXPECT_DOUBLE_EQ(40.0, sza);
  EXPECT_DOUBLE_EQ(0.0, vza);
  EXPECT_DOUBLE_EQ(160.0, raa);

  EXPECT_EQ(0, harp_sensor_geometry_angles_at_altitude_from_other_altitude(
                   5000.0, 35.0, 25.0, 45.0, 5000.0, &sza, &vza, &raa));
  EXPECT_DOUBLE_EQ(35.0, sza);
  EXPECT_DOUBLE_EQ(25.0, vza);
  EXPECT_DOUBLE_EQ(45.0, raa);

  // 2. Non-nadir, target_altitude != source_altitude.
  EXPECT_EQ(0, harp_sensor_geometry_angles_at_altitude_from_other_altitude(
                   0.0, 40.0, 30.0, 60.0, 10000.0, &sza, &vza, &raa));
  EXPECT_GT(sza, 0.0);
  EXPECT_LT(vza, 30.0);
  EXPECT_GT(raa, 0.0);

  // 3. Sun at target zenith (sintheta0k == 0.0): choose theta0 == asin(sinbeta)
  // with raa = 0 so costheta0 * cosbeta + sintheta0 * sinbeta == 1.0.
  const double earth_r = CONST_EARTH_RADIUS_WGS84_SPHERE;
  const double src_vza = 30.0;
  const double src_alt = 0.0;
  const double tgt_alt = -100000.0;
  const double theta_v = src_vza * CONST_DEG2RAD;
  const double theta_vk = std::asin(
      ((earth_r + src_alt) / (earth_r + tgt_alt)) * std::sin(theta_v));
  const double sza_zenith_deg = std::asin(theta_vk - theta_v) * CONST_RAD2DEG;
  EXPECT_EQ(
      0, harp_sensor_geometry_angles_at_altitude_from_other_altitude(
             src_alt, sza_zenith_deg, src_vza, 0.0, tgt_alt, &sza, &vza, &raa));
  EXPECT_NEAR(0.0, sza, 1e-6);
  EXPECT_DOUBLE_EQ(0.0, raa);

  // 4. Profile wrapper: null-pointer validation and valid multi-level output.
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  constexpr auto kAltProfile = std::to_array<double>({0.0, 5000.0, 10000.0});
  std::array<double, 3> sza_prof{};
  std::array<double, 3> vza_prof{};
  std::array<double, 3> raa_prof{};

  EXPECT_EQ(-1, harp_sensor_geometry_angle_profiles_from_sensor_geometry_angles(
                    0.0, 40.0, 30.0, 60.0, 3, nullptr, sza_prof.data(),
                    vza_prof.data(), raa_prof.data()));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  EXPECT_EQ(-1, harp_sensor_geometry_angle_profiles_from_sensor_geometry_angles(
                    0.0, 40.0, 30.0, 60.0, 3, kAltProfile.data(), nullptr,
                    vza_prof.data(), raa_prof.data()));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  EXPECT_EQ(-1, harp_sensor_geometry_angle_profiles_from_sensor_geometry_angles(
                    0.0, 40.0, 30.0, 60.0, 3, kAltProfile.data(),
                    sza_prof.data(), nullptr, raa_prof.data()));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  EXPECT_EQ(-1, harp_sensor_geometry_angle_profiles_from_sensor_geometry_angles(
                    0.0, 40.0, 30.0, 60.0, 3, kAltProfile.data(),
                    sza_prof.data(), vza_prof.data(), nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  EXPECT_EQ(0,
            harp_sensor_geometry_angle_profiles_from_sensor_geometry_angles(
                0.0, 40.0, 30.0, 60.0, kAltProfile.size(), kAltProfile.data(),
                sza_prof.data(), vza_prof.data(), raa_prof.data()));
  EXPECT_DOUBLE_EQ(40.0, sza_prof[0]);
  EXPECT_DOUBLE_EQ(30.0, vza_prof[0]);
  EXPECT_DOUBLE_EQ(60.0, raa_prof[0]);
  EXPECT_LT(vza_prof[2], vza_prof[1]);
}

TEST(HarpAnalysisTest, WrapValueToRange) {
  EXPECT_NEAR(350.0, harp_wrap(-10.0, 0.0, 360.0), 1e-12);
  EXPECT_NEAR(10.0, harp_wrap(370.0, 0.0, 360.0), 1e-12);
  EXPECT_NEAR(180.0, harp_wrap(180.0, 0.0, 360.0), 1e-12);
  EXPECT_NEAR(-90.0, harp_wrap(270.0, -180.0, 180.0), 1e-12);
  EXPECT_NEAR(90.0, harp_wrap(-270.0, -180.0, 180.0), 1e-12);
}

}  // namespace
}  // namespace stcorp_harp
