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
// Unit tests for OSIRIS L2 ingestion modules
// (libharp/harp-ingest-osiris_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-osiris_l2.c

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

constexpr double kSeconds1993To2000 = 220838400.0 + 5.0;

struct OsirisModuleSpec {
  std::string_view module_name;
  std::string_view swath_name;
  std::string_view product_type;
  bool is_aerosol;
  std::string_view vmr_field;
  std::string_view vmr_err_field;
  std::string_view num_dens_field;
  std::string_view harp_vmr_var;
  std::string_view harp_vmr_err_var;
  std::string_view harp_num_dens_var;
};

constexpr OsirisModuleSpec kOsirisModules[] = {
    {.module_name = "OSIRIS_L2_Aerosol_MART",
     .swath_name = "OSIRIS_Odin_Aerosol_MART",
     .product_type = "L2_Aerosol_MART",
     .is_aerosol = true,
     .vmr_field = "Aerosol",
     .vmr_err_field = "AerosolPrecision",
     .num_dens_field = "",
     .harp_vmr_var = "aerosol_number_density",
     .harp_vmr_err_var = "aerosol_number_density_uncertainty",
     .harp_num_dens_var = ""},
    {.module_name = "OSIRIS_L2_NO2_OE",
     .swath_name = "OSIRIS_Odin_NO2_DOAS_OE",
     .product_type = "L2_NO2_OE",
     .is_aerosol = false,
     .vmr_field = "NO2",
     .vmr_err_field = "NO2Precision",
     .num_dens_field = "NO2NumberDensity",
     .harp_vmr_var = "no2_volume_mixing_ratio",
     .harp_vmr_err_var = "no2_volume_mixing_ratio_uncertainty",
     .harp_num_dens_var = "no2_number_density"},
    {.module_name = "OSIRIS_L2_NO2_MART",
     .swath_name = "OSIRIS_Odin_NO2MART",
     .product_type = "L2_NO2_MART",
     .is_aerosol = false,
     .vmr_field = "NO2",
     .vmr_err_field = "NO2Precision",
     .num_dens_field = "NO2NumberDensity",
     .harp_vmr_var = "no2_volume_mixing_ratio",
     .harp_vmr_err_var = "no2_volume_mixing_ratio_uncertainty",
     .harp_num_dens_var = "no2_number_density"},
    {.module_name = "OSIRIS_L2_O3_OE",
     .swath_name = "OSIRIS_Odin_O3_Chappuis_triplet_OE",
     .product_type = "L2_O3_OE",
     .is_aerosol = false,
     .vmr_field = "O3",
     .vmr_err_field = "O3Precision",
     .num_dens_field = "O3NumberDensity",
     .harp_vmr_var = "o3_volume_mixing_ratio",
     .harp_vmr_err_var = "o3_volume_mixing_ratio_uncertainty",
     .harp_num_dens_var = "o3_number_density"},
    {.module_name = "OSIRIS_L2_O3_MART",
     .swath_name = "OSIRIS_Odin_O3MART",
     .product_type = "L2_O3_MART",
     .is_aerosol = false,
     .vmr_field = "O3",
     .vmr_err_field = "O3Precision",
     .num_dens_field = "O3NumberDensity",
     .harp_vmr_var = "o3_volume_mixing_ratio",
     .harp_vmr_err_var = "o3_volume_mixing_ratio_uncertainty",
     .harp_num_dens_var = "o3_number_density"},
};

void PopulateOsirisProduct(CodaMemProduct& prod, const OsirisModuleSpec& spec) {
  ASSERT_TRUE(prod.SetProductInfo("ODIN_OSIRIS", spec.product_type, 1));

  CodaMemNode* hdfeos = prod.MakeRecord();
  CodaMemNode* swaths = prod.MakeRecord();
  CodaMemNode* swath = prod.MakeRecord();
  CodaMemNode* geo = prod.MakeRecord();
  CodaMemNode* data = prod.MakeRecord();
  ASSERT_NE(nullptr, hdfeos);
  ASSERT_NE(nullptr, swaths);
  ASSERT_NE(nullptr, swath);
  ASSERT_NE(nullptr, geo);
  ASSERT_NE(nullptr, data);

  constexpr long kProfDims[] = {2};
  constexpr double kTimes[] = {kSeconds1993To2000 + 10.0,
                               kSeconds1993To2000 + 20.0};
  constexpr double kLats[] = {-45.0, 45.0};
  constexpr double kLons[] = {-90.0, 90.0};
  constexpr double kSza[] = {35.0, 40.0};
  constexpr double kSaa[] = {110.0, 120.0};
  ASSERT_TRUE(
      prod.AddField(geo, "Time", prod.MakeDoubleArray(kProfDims, kTimes)));
  ASSERT_TRUE(
      prod.AddField(geo, "Latitude", prod.MakeDoubleArray(kProfDims, kLats)));
  ASSERT_TRUE(
      prod.AddField(geo, "Longitude", prod.MakeDoubleArray(kProfDims, kLons)));
  ASSERT_TRUE(prod.AddField(geo, "SolarZenithAngle",
                            prod.MakeDoubleArray(kProfDims, kSza)));
  ASSERT_TRUE(prod.AddField(geo, "SolarAzimuthAngle",
                            prod.MakeDoubleArray(kProfDims, kSaa)));

  constexpr long kAltDims[] = {3};
  constexpr double kAlts[] = {10.5, 20.5, 30.5};
  ASSERT_TRUE(
      prod.AddField(geo, "Altitude", prod.MakeDoubleArray(kAltDims, kAlts)));

  constexpr long kGridDims[] = {2, 3};
  constexpr double kVals[] = {1.1, 1.2, 1.3, 2.1, 2.2, 2.3};
  constexpr double kErrs[] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6};
  ASSERT_TRUE(prod.AddField(data, spec.vmr_field,
                            prod.MakeDoubleArray(kGridDims, kVals)));
  ASSERT_TRUE(prod.AddField(data, spec.vmr_err_field,
                            prod.MakeDoubleArray(kGridDims, kErrs)));
  if (!spec.is_aerosol) {
    constexpr double kNd[] = {1e10, 2e10, 3e10, 4e10, 5e10, 6e10};
    ASSERT_TRUE(prod.AddField(data, spec.num_dens_field,
                              prod.MakeDoubleArray(kGridDims, kNd)));
  }

  ASSERT_TRUE(prod.AddField(swath, "Geolocation_Fields", geo));
  ASSERT_TRUE(prod.AddField(swath, "Data_Fields", data));
  ASSERT_TRUE(prod.AddField(swaths, spec.swath_name, swath));
  ASSERT_TRUE(prod.AddField(hdfeos, "SWATHS", swaths));
  ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
}

TEST(HarpIngestOsirisL2Test, IngestAllFiveProductTypes) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  for (const auto& spec : kOsirisModules) {
    SCOPED_TRACE(spec.module_name);
    const harp_ingestion_module* mod = FindIngestionModule(spec.module_name);
    ASSERT_NE(nullptr, mod);

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateOsirisProduct(prod, spec);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(3, res.dimensions[harp_dimension_vertical]);

    EXPECT_THAT(res.variables["datetime"].double_values,
                ElementsAre(10.0, 20.0));
    EXPECT_THAT(res.variables["latitude"].double_values,
                ElementsAre(-45.0, 45.0));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(-90.0, 90.0));
    EXPECT_THAT(res.variables["altitude"].double_values,
                ElementsAre(10.5, 20.5, 30.5, 10.5, 20.5, 30.5));
    EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
                ElementsAre(35.0, 40.0));
    EXPECT_THAT(res.variables["solar_azimuth_angle"].double_values,
                ElementsAre(110.0, 120.0));
    EXPECT_THAT(res.variables[std::string(spec.harp_vmr_var)].double_values,
                ElementsAre(1.1, 1.2, 1.3, 2.1, 2.2, 2.3));
    EXPECT_THAT(res.variables[std::string(spec.harp_vmr_err_var)].double_values,
                ElementsAre(0.1, 0.2, 0.3, 0.4, 0.5, 0.6));
    if (!spec.is_aerosol) {
      EXPECT_THAT(
          res.variables[std::string(spec.harp_num_dens_var)].double_values,
          ElementsAre(1e10, 2e10, 3e10, 4e10, 5e10, 6e10));
    }
  }
}

TEST(HarpIngestOsirisL2Test, ErrorPathsMissingAndInvalidNodes) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod =
      FindIngestionModule("OSIRIS_L2_Aerosol_MART");
  ASSERT_NE(nullptr, mod);

  // 1. Missing SetProductInfo -> coda_get_product_version fails.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Missing /HDFEOS/SWATHS.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ODIN_OSIRIS", "L2_Aerosol_MART", 1));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Missing swath under /HDFEOS/SWATHS.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ODIN_OSIRIS", "L2_Aerosol_MART", 1));
    CodaMemNode* hdfeos = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(hdfeos, "SWATHS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 4. Missing Geolocation_Fields, missing Latitude, missing Altitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ODIN_OSIRIS", "L2_Aerosol_MART", 1));
    CodaMemNode* hdfeos = prod.MakeRecord();
    CodaMemNode* swaths = prod.MakeRecord();
    CodaMemNode* swath = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(swaths, "OSIRIS_Odin_Aerosol_MART", swath));
    ASSERT_TRUE(prod.AddField(hdfeos, "SWATHS", swaths));
    ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);

    CodaMemProduct prod2;
    ASSERT_TRUE(prod2.ok());
    ASSERT_TRUE(prod2.SetProductInfo("ODIN_OSIRIS", "L2_Aerosol_MART", 1));
    CodaMemNode* hdfeos2 = prod2.MakeRecord();
    CodaMemNode* swaths2 = prod2.MakeRecord();
    CodaMemNode* swath2 = prod2.MakeRecord();
    CodaMemNode* geo2 = prod2.MakeRecord();
    ASSERT_TRUE(prod2.AddField(swath2, "Geolocation_Fields", geo2));
    ASSERT_TRUE(prod2.AddField(swaths2, "OSIRIS_Odin_Aerosol_MART", swath2));
    ASSERT_TRUE(prod2.AddField(hdfeos2, "SWATHS", swaths2));
    ASSERT_TRUE(prod2.AddField(prod2.root(), "HDFEOS", hdfeos2));
    res = ExerciseIngestionModule(mod, prod2.product());
    EXPECT_EQ(-1, res.status);

    CodaMemProduct prod3;
    ASSERT_TRUE(prod3.ok());
    ASSERT_TRUE(prod3.SetProductInfo("ODIN_OSIRIS", "L2_Aerosol_MART", 1));
    CodaMemNode* hdfeos3 = prod3.MakeRecord();
    CodaMemNode* swaths3 = prod3.MakeRecord();
    CodaMemNode* swath3 = prod3.MakeRecord();
    CodaMemNode* geo3 = prod3.MakeRecord();
    constexpr long kDims[] = {1};
    constexpr double kOneLat[] = {10.0};
    ASSERT_TRUE(prod3.AddField(geo3, "Latitude",
                               prod3.MakeDoubleArray(kDims, kOneLat)));
    ASSERT_TRUE(prod3.AddField(swath3, "Geolocation_Fields", geo3));
    ASSERT_TRUE(prod3.AddField(swaths3, "OSIRIS_Odin_Aerosol_MART", swath3));
    ASSERT_TRUE(prod3.AddField(hdfeos3, "SWATHS", swaths3));
    ASSERT_TRUE(prod3.AddField(prod3.root(), "HDFEOS", hdfeos3));
    res = ExerciseIngestionModule(mod, prod3.product());
    EXPECT_EQ(-1, res.status);
  }

  // 5. Missing Data_Fields / wrong-type Time during variable read.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ODIN_OSIRIS", "L2_Aerosol_MART", 1));
    CodaMemNode* hdfeos = prod.MakeRecord();
    CodaMemNode* swaths = prod.MakeRecord();
    CodaMemNode* swath = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    constexpr long kDims[] = {1};
    constexpr double kOneLat[] = {10.0};
    constexpr double kOneAlt[] = {20.0};
    constexpr std::string_view kBadTime[] = {"bad"};
    ASSERT_TRUE(
        prod.AddField(geo, "Time", prod.MakeStringArray(kDims, kBadTime)));
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(kDims, kOneLat)));
    ASSERT_TRUE(
        prod.AddField(geo, "Altitude", prod.MakeDoubleArray(kDims, kOneAlt)));
    ASSERT_TRUE(prod.AddField(swath, "Geolocation_Fields", geo));
    ASSERT_TRUE(prod.AddField(swaths, "OSIRIS_Odin_Aerosol_MART", swath));
    ASSERT_TRUE(prod.AddField(hdfeos, "SWATHS", swaths));
    ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["aerosol_number_density"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
