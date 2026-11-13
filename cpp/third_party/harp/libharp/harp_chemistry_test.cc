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
// Unit tests for chemical species metadata and atmospheric chemistry
// conversions (libharp/harp-chemistry.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-chemistry.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-chemistry.h

#include <array>
#include <cmath>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

TEST(HarpChemistryTest, SpeciesNameAndMolarMassForAllValidEnums) {
  // Verify every valid enum value [0 .. harp_chemical_species_unknown] returns
  // a non-empty name and expected molar mass (> 0 for known species, 0 for
  // unknown). Never pass out-of-range enums due to internal assert().
  for (int i = 0; i <= static_cast<int>(harp_chemical_species_unknown); ++i) {
    const auto species = static_cast<harp_chemical_species>(i);
    const char* name = harp_chemical_species_name(species);
    ASSERT_NE(nullptr, name);
    EXPECT_NE(std::string_view(""), name);

    const double molar_mass = harp_molar_mass_for_species(species);
    if (species == harp_chemical_species_unknown) {
      EXPECT_DOUBLE_EQ(0.0, molar_mass);
    } else {
      EXPECT_GT(molar_mass, 0.0) << "species=" << name;
    }
  }

  EXPECT_STREQ("dry_air",
               harp_chemical_species_name(harp_chemical_species_dry_air));
  EXPECT_STREQ("O3", harp_chemical_species_name(harp_chemical_species_O3));
  EXPECT_STREQ("unknown",
               harp_chemical_species_name(harp_chemical_species_unknown));
  EXPECT_DOUBLE_EQ(CONST_MOLAR_MASS_DRY_AIR,
                   harp_molar_mass_for_species(harp_chemical_species_dry_air));
  EXPECT_DOUBLE_EQ(CONST_MOLAR_MASS_O3,
                   harp_molar_mass_for_species(harp_chemical_species_O3));
  EXPECT_DOUBLE_EQ(CONST_MOLAR_MASS_H2O, harp_molar_mass_for_species(
                                             harp_chemical_species_ice_water));
}

TEST(HarpChemistryTest, SpeciesFromVariableNameWithAndWithoutPrefixes) {
  EXPECT_EQ(harp_chemical_species_unknown,
            harp_chemical_species_from_variable_name(nullptr));
  EXPECT_EQ(harp_chemical_species_unknown,
            harp_chemical_species_from_variable_name(""));
  EXPECT_EQ(harp_chemical_species_unknown,
            harp_chemical_species_from_variable_name("O3"));
  EXPECT_EQ(harp_chemical_species_unknown,
            harp_chemical_species_from_variable_name("O3x_column"));
  EXPECT_EQ(harp_chemical_species_unknown,
            harp_chemical_species_from_variable_name("unknown_density"));
  EXPECT_EQ(harp_chemical_species_unknown,
            harp_chemical_species_from_variable_name("surface_altitude"));

  // Round-trip every known species with and without domain prefixes.
  constexpr auto kPrefixes = std::to_array<std::string_view>({
      "",
      "surface_",
      "tropospheric_",
      "stratospheric_",
  });
  for (int i = 0; i < static_cast<int>(harp_num_chemical_species); ++i) {
    const auto species = static_cast<harp_chemical_species>(i);
    const char* name = harp_chemical_species_name(species);
    for (const std::string_view prefix : kPrefixes) {
      const std::string var_name =
          std::string(prefix) + name + "_number_density";
      // Linear scan in harp_chemical_species_from_variable_name checks
      // `strncmp(..., name, len) == 0 && var_name[len] == '_'`, so isotopologue
      // names starting with "H2O_" or "O3_" match the earlier base species
      // ("H2O" / "O3") in chemical_species_names[].
      harp_chemical_species expected = species;
      if (species == harp_chemical_species_H2O_161 ||
          species == harp_chemical_species_H2O_162 ||
          species == harp_chemical_species_H2O_171 ||
          species == harp_chemical_species_H2O_181) {
        expected = harp_chemical_species_H2O;
      } else if (species == harp_chemical_species_O3_666 ||
                 species == harp_chemical_species_O3_667 ||
                 species == harp_chemical_species_O3_668 ||
                 species == harp_chemical_species_O3_686) {
        expected = harp_chemical_species_O3;
      }
      EXPECT_EQ(expected,
                harp_chemical_species_from_variable_name(var_name.c_str()))
          << "var_name=" << var_name;
    }
  }
}

TEST(HarpChemistryTest, PartialColumnAndDensityBoundsConversions) {
  constexpr double kAscBounds[] = {1000.0, 2500.0};
  constexpr double kDescBounds[] = {2500.0, 1000.0};
  constexpr double kZeroBounds[] = {1000.0, 1000.0};
  constexpr double kTinyBounds[] = {1000.0, 1000.0 + 1e-12};

  EXPECT_DOUBLE_EQ(0.02, harp_density_from_partial_column_and_altitude_bounds(
                             30.0, kAscBounds));
  EXPECT_DOUBLE_EQ(0.02, harp_density_from_partial_column_and_altitude_bounds(
                             30.0, kDescBounds));
  // Height < EPSILON (1e-10) returns 0 to avoid division by zero.
  EXPECT_DOUBLE_EQ(0.0, harp_density_from_partial_column_and_altitude_bounds(
                            30.0, kZeroBounds));
  EXPECT_DOUBLE_EQ(0.0, harp_density_from_partial_column_and_altitude_bounds(
                            30.0, kTinyBounds));

  EXPECT_DOUBLE_EQ(30.0, harp_partial_column_from_density_and_altitude_bounds(
                             0.02, kAscBounds));
  EXPECT_DOUBLE_EQ(30.0, harp_partial_column_from_density_and_altitude_bounds(
                             0.02, kDescBounds));
}

TEST(HarpChemistryTest, MassNumberDensityAndMixingRatioConversions) {
  constexpr double kNumDensity = 2.5e25;
  constexpr double kMolarMass = CONST_MOLAR_MASS_O3;
  const double mass_density =
      harp_mass_density_from_number_density(kNumDensity, kMolarMass);
  EXPECT_DOUBLE_EQ(1e-3 * kNumDensity * kMolarMass / CONST_NUM_AVOGADRO,
                   mass_density);
  EXPECT_NEAR(kNumDensity,
              harp_number_density_from_mass_density(mass_density, kMolarMass),
              1e10);

  EXPECT_DOUBLE_EQ(0.25, harp_mass_mixing_ratio_from_density(0.3, 1.2));

  constexpr double kVmr = 1e-6;
  const double mmr = harp_mass_mixing_ratio_from_volume_mixing_ratio(
      kVmr, CONST_MOLAR_MASS_O3, CONST_MOLAR_MASS_DRY_AIR);
  EXPECT_DOUBLE_EQ(kVmr * CONST_MOLAR_MASS_O3 / CONST_MOLAR_MASS_DRY_AIR, mmr);
  EXPECT_NEAR(kVmr,
              harp_volume_mixing_ratio_from_mass_mixing_ratio(
                  mmr, CONST_MOLAR_MASS_O3, CONST_MOLAR_MASS_DRY_AIR),
              1e-20);

  constexpr double kAirNd = 2.5e25;
  const double species_nd =
      harp_number_density_from_volume_mixing_ratio(kVmr, kAirNd);
  EXPECT_DOUBLE_EQ(kVmr * kAirNd, species_nd);
  EXPECT_NEAR(kVmr,
              harp_volume_mixing_ratio_from_number_density(species_nd, kAirNd),
              1e-20);
}

TEST(HarpChemistryTest, MolarMassOfWetAirAndVirtualTemperature) {
  constexpr double kAirDensity = 1.225;
  constexpr double kAirNd = 2.545818e25;
  EXPECT_DOUBLE_EQ(
      1e3 * kAirDensity * CONST_NUM_AVOGADRO / kAirNd,
      harp_molar_mass_air_from_density_and_number_density(kAirDensity, kAirNd));

  constexpr double kH2oMmr = 0.01;
  const double expected_mm_from_mmr =
      (CONST_MOLAR_MASS_DRY_AIR * CONST_MOLAR_MASS_H2O) /
      ((1.0 - kH2oMmr) * CONST_MOLAR_MASS_H2O +
       kH2oMmr * CONST_MOLAR_MASS_DRY_AIR);
  EXPECT_DOUBLE_EQ(expected_mm_from_mmr,
                   harp_molar_mass_air_from_h2o_mass_mixing_ratio(kH2oMmr));

  constexpr double kH2oVmr = 0.015;
  EXPECT_DOUBLE_EQ(CONST_MOLAR_MASS_DRY_AIR * (1.0 - kH2oVmr) +
                       CONST_MOLAR_MASS_H2O * kH2oVmr,
                   harp_molar_mass_air_from_h2o_volume_mixing_ratio(kH2oVmr));

  constexpr double kTemp = 288.15;
  const double virt_temp =
      harp_virtual_temperature_from_temperature(kTemp, expected_mm_from_mmr);
  EXPECT_DOUBLE_EQ(kTemp * CONST_MOLAR_MASS_DRY_AIR / expected_mm_from_mmr,
                   virt_temp);
  EXPECT_NEAR(kTemp,
              harp_temperature_from_virtual_temperature(virt_temp,
                                                        expected_mm_from_mmr),
              1e-12);
}

TEST(HarpChemistryTest, IdealGasPartialPressureHumidityAndColumnVmr) {
  constexpr double kPressure = 101325.0;
  constexpr double kTemp = 293.15;
  const double nd =
      harp_number_density_from_pressure_and_temperature(kPressure, kTemp);
  EXPECT_DOUBLE_EQ(kPressure / (CONST_BOLTZMANN * kTemp), nd);
  EXPECT_NEAR(kPressure,
              harp_pressure_from_number_density_and_temperature(nd, kTemp),
              1e-9);
  EXPECT_NEAR(kTemp,
              harp_temperature_from_number_density_and_pressure(nd, kPressure),
              1e-12);

  constexpr double kVmr = 0.02;
  const double partial_p =
      harp_partial_pressure_from_volume_mixing_ratio_and_pressure(kVmr,
                                                                  kPressure);
  EXPECT_DOUBLE_EQ(kVmr * kPressure, partial_p);
  EXPECT_NEAR(kVmr,
              harp_volume_mixing_ratio_from_partial_pressure_and_pressure(
                  partial_p, kPressure),
              1e-15);

  // Relative humidity at 20 C (293.15 K) against August-Roche-Magnus formula.
  constexpr double kH2oPartialP = 1169.0;
  const double temp_c = kTemp - 273.15;
  const double sat_p = 610.94 * std::exp(17.625 * temp_c / (temp_c + 243.04));
  EXPECT_DOUBLE_EQ(
      kH2oPartialP / sat_p,
      harp_relative_humidity_from_h2o_partial_pressure_and_temperature(
          kH2oPartialP, kTemp));

  // Partial column from VMR and pressure bounds.
  constexpr double kPressureBounds[] = {101325.0, 50000.0};
  const double p_mid = std::exp(
      0.5 * (std::log(kPressureBounds[0]) + std::log(kPressureBounds[1])));
  const double z = -(CONST_MOLAR_GAS * CONST_STD_TEMPERATURE) *
                   std::log(p_mid / CONST_STD_PRESSURE) /
                   (1e-3 * CONST_MOLAR_MASS_DRY_AIR * CONST_GRAV_ACCEL);
  const double g = harp_gravity_from_latitude_and_altitude(45.0, z);
  const double expected_col = -kVmr * CONST_NUM_AVOGADRO *
                              (kPressureBounds[1] - kPressureBounds[0]) /
                              (1e-3 * CONST_MOLAR_MASS_DRY_AIR * g);
  EXPECT_DOUBLE_EQ(expected_col,
                   harp_partial_column_number_density_from_volume_mixing_ratio(
                       kVmr, 45.0, CONST_MOLAR_MASS_DRY_AIR, kPressureBounds));
}

}  // namespace
}  // namespace stcorp_harp
