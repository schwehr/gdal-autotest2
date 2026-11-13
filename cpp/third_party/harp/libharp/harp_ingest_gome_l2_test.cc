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
// Unit tests for GOME Level 2 ingestion (libharp/harp-ingest-gome_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-gome_l2.c

#include <cstdint>
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

using ::testing::DoubleEq;
using ::testing::DoubleNear;
using ::testing::ElementsAre;

CodaMemNode* MakeSingleNodeArray(CodaMemProduct& prod, CodaMemNode* elem) {
  constexpr long kOne[] = {1};
  CodaMemNode* elems[] = {elem};
  return prod.MakeNodeArray(kOne, elems);
}

struct DdrSpec {
  double datetime;
  int32_t subset_counter;
  double corner_lats[5];
  double corner_lons[5];
  double sza_toa[2];
  double los_toa[2];
  double rel_azi_toa[2];
  double vcd_total[3];
  double vcd_error[3];
  double cloud_frac;
  double cloud_frac_err;
  double cloud_press;
  double cloud_press_err;
  double cloud_height;
  double cloud_height_err;
  double cloud_albedo;
  double cloud_albedo_err;
  double surf_press;
  double surf_height;
  double surf_albedo;
};

CodaMemNode* MakeCornerRecord(CodaMemProduct& prod, double lat, double lon) {
  CodaMemNode* rec = prod.MakeRecord();
  // lat must be field index 0 and lon must be field index 1 because
  // read_latitude_bounds uses coda_cursor_goto_first_record_field and
  // read_longitude_bounds uses coda_cursor_goto_record_field_by_index(..., 1).
  if (rec == nullptr || !prod.AddField(rec, "lat", prod.MakeDouble(lat)) ||
      !prod.AddField(rec, "lon", prod.MakeDouble(lon))) {
    return nullptr;
  }
  return rec;
}

CodaMemNode* MakeDdrRecord(CodaMemProduct& prod, int version,
                           const DdrSpec& spec) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr) {
    return nullptr;
  }

  // glr record.
  CodaMemNode* glr = prod.MakeRecord();
  if (glr == nullptr ||
      !prod.AddField(glr, "datetime", prod.MakeDouble(spec.datetime)) ||
      !prod.AddField(glr, "subset_counter",
                     prod.MakeInt32(spec.subset_counter))) {
    return nullptr;
  }
  std::vector<CodaMemNode*> corners;
  for (int i = 0; i < 5; ++i) {
    corners.push_back(
        MakeCornerRecord(prod, spec.corner_lats[i], spec.corner_lons[i]));
  }
  const long five_dims[] = {5};
  const long two_dims[] = {2};
  if (!prod.AddField(glr, "corners", prod.MakeNodeArray(five_dims, corners)) ||
      !prod.AddField(glr, "sza_toa",
                     prod.MakeDoubleArray(two_dims, spec.sza_toa)) ||
      !prod.AddField(glr, "line_sight_toa",
                     prod.MakeDoubleArray(two_dims, spec.los_toa)) ||
      !prod.AddField(glr, "rel_azi_toa",
                     prod.MakeDoubleArray(two_dims, spec.rel_azi_toa)) ||
      !prod.AddField(rec, "glr", glr)) {
    return nullptr;
  }

  // irr record.
  CodaMemNode* irr = prod.MakeRecord();
  CodaMemNode* vcd = prod.MakeRecord();
  const long three_dims[] = {3};
  if (irr == nullptr || vcd == nullptr ||
      !prod.AddField(vcd, "total",
                     prod.MakeDoubleArray(three_dims, spec.vcd_total)) ||
      !prod.AddField(vcd, "error",
                     prod.MakeDoubleArray(three_dims, spec.vcd_error)) ||
      !prod.AddField(irr, "vcd", vcd)) {
    return nullptr;
  }

  if (version == 1) {
    CodaMemNode* icfa = prod.MakeRecord();
    if (icfa == nullptr ||
        !prod.AddField(icfa, "frac", prod.MakeDouble(spec.cloud_frac)) ||
        !prod.AddField(icfa, "err_frac",
                       prod.MakeDouble(spec.cloud_frac_err)) ||
        !prod.AddField(icfa, "press", prod.MakeDouble(spec.cloud_press)) ||
        !prod.AddField(icfa, "err_press",
                       prod.MakeDouble(spec.cloud_press_err)) ||
        !prod.AddField(icfa, "surf_press", prod.MakeDouble(spec.surf_press)) ||
        !prod.AddField(irr, "icfa", icfa)) {
      return nullptr;
    }
  } else {
    CodaMemNode* ocra = prod.MakeRecord();
    CodaMemNode* rocinn = prod.MakeRecord();
    if (ocra == nullptr || rocinn == nullptr ||
        !prod.AddField(ocra, "cloud_frac", prod.MakeDouble(spec.cloud_frac)) ||
        !prod.AddField(ocra, "cloud_frac_error",
                       prod.MakeDouble(spec.cloud_frac_err)) ||
        !prod.AddField(irr, "ocra", ocra) ||
        !prod.AddField(rocinn, "height", prod.MakeDouble(spec.cloud_height)) ||
        !prod.AddField(rocinn, "height_error",
                       prod.MakeDouble(spec.cloud_height_err)) ||
        !prod.AddField(rocinn, "pressure", prod.MakeDouble(spec.cloud_press)) ||
        !prod.AddField(rocinn, "pressure_error",
                       prod.MakeDouble(spec.cloud_press_err)) ||
        !prod.AddField(rocinn, "albedo", prod.MakeDouble(spec.cloud_albedo)) ||
        !prod.AddField(rocinn, "albedo_error",
                       prod.MakeDouble(spec.cloud_albedo_err)) ||
        !prod.AddField(irr, "rocinn", rocinn) ||
        !prod.AddField(irr, "surface_pressure",
                       prod.MakeDouble(spec.surf_press)) ||
        !prod.AddField(irr, "surface_height",
                       prod.MakeDouble(spec.surf_height)) ||
        !prod.AddField(irr, "surface_albedo",
                       prod.MakeDouble(spec.surf_albedo))) {
      return nullptr;
    }
  }

  if (!prod.AddField(rec, "irr", irr)) {
    return nullptr;
  }
  return rec;
}

void PopulateGomeL2Product(CodaMemProduct& prod, int version, int32_t orbit,
                           const std::vector<DdrSpec>& ddrs) {
  ASSERT_TRUE(prod.SetProductInfo("ERS_GOME", "GOM_LVL21", version));
  CodaMemNode* pir = prod.MakeRecord();
  ASSERT_NE(nullptr, pir);
  ASSERT_TRUE(prod.AddField(pir, "start_orbit", prod.MakeInt32(orbit)));
  ASSERT_TRUE(prod.AddField(prod.root(), "pir", pir));

  std::vector<CodaMemNode*> ddr_nodes;
  ddr_nodes.reserve(ddrs.size());
  for (const auto& d : ddrs) {
    ddr_nodes.push_back(MakeDdrRecord(prod, version, d));
  }
  const long ddr_dims[] = {static_cast<long>(ddr_nodes.size())};
  DdrSpec sample = {};
  CodaMemNode* sample_node =
      ddr_nodes.empty() ? MakeDdrRecord(prod, version, sample) : nullptr;
  ASSERT_TRUE(
      prod.AddField(prod.root(), "ddr",
                    prod.MakeNodeArray(ddr_dims, ddr_nodes, sample_node)));
}

TEST(HarpIngestGomeL2Test, IngestVersion1Success) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOME_L2");
  ASSERT_NE(nullptr, mod);

  std::vector<DdrSpec> ddrs = {
      {
          .datetime = 500.0,
          .subset_counter = 1,
          .corner_lats = {10.0, 11.0, 12.0, 13.0, 11.5},
          // Corner 1 > 180 tests longitude normalization (200 -> -160).
          .corner_lons = {170.0, 200.0, 175.0, 185.0, 178.0},
          .sza_toa = {30.0, 31.0},
          .los_toa = {5.0, 6.0},
          .rel_azi_toa = {40.0, 41.0},
          .vcd_total = {300.0, 310.0, 5.0},
          .vcd_error = {10.0, 20.0, 50.0},
          .cloud_frac = 0.4,
          .cloud_frac_err = 25.0,
          .cloud_press = 600.0,
          .cloud_press_err = 10.0,
          .surf_press = 1010.0,
      },
      {
          .datetime = 501.5,
          .subset_counter = 3,
          .corner_lats = {20.0, 21.0, 22.0, 23.0, 21.5},
          .corner_lons = {-10.0, -9.0, -8.0, -7.0, -8.5},
          .sza_toa = {32.0, 33.0},
          .los_toa = {7.0, 8.0},
          .rel_azi_toa = {42.0, 43.0},
          .vcd_total = {320.0, 330.0, 6.0},
          .vcd_error = {5.0, 15.0, 25.0},
          .cloud_frac = 0.8,
          .cloud_frac_err = 10.0,
          .cloud_press = 500.0,
          .cloud_press_err = 20.0,
          .surf_press = 1005.0,
      },
  };

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateGomeL2Product(prod, /*version=*/1, /*orbit=*/1234, ddrs);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);

  EXPECT_FALSE(res.variables["cloud_top_height"].included);
  EXPECT_FALSE(res.variables["cloud_top_height_uncertainty"].included);
  EXPECT_FALSE(res.variables["cloud_top_albedo"].included);
  EXPECT_FALSE(res.variables["cloud_top_albedo_uncertainty"].included);
  EXPECT_FALSE(res.variables["surface_height"].included);
  EXPECT_FALSE(res.variables["surface_albedo"].included);

  EXPECT_THAT(res.variables["datetime_stop"].double_values,
              ElementsAre(500.0, 501.5));
  EXPECT_THAT(res.variables["datetime_length"].double_values,
              ElementsAre(1.5, 1.5));
  EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(1234));
  EXPECT_THAT(res.variables["latitude"].double_values, ElementsAre(11.5, 21.5));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(178.0, -8.5));
  EXPECT_THAT(res.variables["latitude_bounds"].double_values,
              ElementsAre(11.0, 13.0, 12.0, 10.0, 21.0, 23.0, 22.0, 20.0));
  EXPECT_THAT(
      res.variables["longitude_bounds"].double_values,
      ElementsAre(-160.0, -175.0, 175.0, 170.0, -9.0, -7.0, -8.0, -10.0));
  EXPECT_THAT(res.variables["O3_column_number_density"].double_values,
              ElementsAre(300.0, 320.0));
  EXPECT_THAT(
      res.variables["O3_column_number_density_uncertainty"].double_values,
      ElementsAre(DoubleNear(30.0, 1e-12), DoubleNear(16.0, 1e-12)));
  EXPECT_THAT(res.variables["NO2_column_number_density"].double_values,
              ElementsAre(5.0, 6.0));
  EXPECT_THAT(
      res.variables["NO2_column_number_density_uncertainty"].double_values,
      ElementsAre(DoubleNear(2.5, 1e-12), DoubleNear(1.5, 1e-12)));
  EXPECT_THAT(res.variables["cloud_fraction"].double_values,
              ElementsAre(0.4, 0.8));
  EXPECT_THAT(res.variables["cloud_fraction_uncertainty"].double_values,
              ElementsAre(DoubleNear(0.1, 1e-12), DoubleNear(0.08, 1e-12)));
  EXPECT_THAT(res.variables["cloud_top_pressure"].double_values,
              ElementsAre(600.0, 500.0));
  EXPECT_THAT(res.variables["cloud_top_pressure_uncertainty"].double_values,
              ElementsAre(DoubleNear(60.0, 1e-12), DoubleNear(100.0, 1e-12)));
  EXPECT_THAT(res.variables["surface_pressure"].double_values,
              ElementsAre(1010.0, 1005.0));
  EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
              ElementsAre(31.0, 33.0));
  EXPECT_THAT(res.variables["viewing_zenith_angle"].double_values,
              ElementsAre(6.0, 8.0));
  EXPECT_THAT(res.variables["relative_azimuth_angle"].double_values,
              ElementsAre(41.0, 43.0));
  EXPECT_THAT(res.variables["scan_subindex"].int8_values, ElementsAre(1, 3));
  EXPECT_THAT(res.variables["scan_direction_type"].int8_values,
              ElementsAre(0, 1));
}

TEST(HarpIngestGomeL2Test, IngestVersion2WithOzoneVcd1Option) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOME_L2");
  ASSERT_NE(nullptr, mod);

  std::vector<DdrSpec> ddrs = {
      {
          .datetime = 700.0,
          .subset_counter = 2,
          .corner_lats = {0.0, 1.0, 2.0, 3.0, 1.5},
          .corner_lons = {0.0, 1.0, 2.0, 3.0, 1.5},
          .sza_toa = {20.0, 21.0},
          .los_toa = {10.0, 11.0},
          .rel_azi_toa = {50.0, 51.0},
          .vcd_total = {300.0, 350.0, 4.0},
          .vcd_error = {10.0, 20.0, 10.0},
          .cloud_frac = 0.5,
          .cloud_frac_err = 10.0,
          .cloud_press = 700.0,
          .cloud_press_err = 5.0,
          .cloud_height = 4.0,
          .cloud_height_err = 25.0,
          .cloud_albedo = 0.6,
          .cloud_albedo_err = 10.0,
          .surf_press = 1013.0,
          .surf_height = 0.2,
          .surf_albedo = 0.15,
      },
  };

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateGomeL2Product(prod, /*version=*/2, /*orbit=*/99, ddrs);

  IngestExerciseResult res =
      ExerciseIngestionModule(mod, prod.product(), "ozone=vcd1");
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_TRUE(res.variables["cloud_top_height"].included);
  EXPECT_THAT(res.variables["O3_column_number_density"].double_values,
              ElementsAre(350.0));
  EXPECT_THAT(
      res.variables["O3_column_number_density_uncertainty"].double_values,
      ElementsAre(DoubleNear(70.0, 1e-12)));
  EXPECT_THAT(res.variables["cloud_fraction"].double_values, ElementsAre(0.5));
  EXPECT_THAT(res.variables["cloud_fraction_uncertainty"].double_values,
              ElementsAre(DoubleNear(0.05, 1e-12)));
  EXPECT_THAT(res.variables["cloud_top_height"].double_values,
              ElementsAre(4.0));
  EXPECT_THAT(res.variables["cloud_top_height_uncertainty"].double_values,
              ElementsAre(DoubleNear(1.0, 1e-12)));
  EXPECT_THAT(res.variables["cloud_top_pressure"].double_values,
              ElementsAre(700.0));
  EXPECT_THAT(res.variables["cloud_top_pressure_uncertainty"].double_values,
              ElementsAre(DoubleNear(35.0, 1e-12)));
  EXPECT_THAT(res.variables["cloud_top_albedo"].double_values,
              ElementsAre(0.6));
  EXPECT_THAT(res.variables["cloud_top_albedo_uncertainty"].double_values,
              ElementsAre(DoubleNear(0.06, 1e-12)));
  EXPECT_THAT(res.variables["surface_pressure"].double_values,
              ElementsAre(1013.0));
  EXPECT_THAT(res.variables["surface_height"].double_values, ElementsAre(0.2));
  EXPECT_THAT(res.variables["surface_albedo"].double_values, ElementsAre(0.15));
}

TEST(HarpIngestGomeL2Test, IngestEmptyDdrSucceeds) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOME_L2");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateGomeL2Product(prod, /*version=*/2, /*orbit=*/5, {});

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(0, res.dimensions[harp_dimension_time]);
  EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(5));
}

TEST(HarpIngestGomeL2Test, ErrorPathsMissingAndInvalidFields) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOME_L2");
  ASSERT_NE(nullptr, mod);

  // 1. Missing ddr in init_ddr_cursor.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ERS_GOME", "GOM_LVL21", 2));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }

  // 2. Empty ddr record triggers missing field errors across readers.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ERS_GOME", "GOM_LVL21", 2));
    ASSERT_TRUE(prod.AddField(prod.root(), "ddr",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
    EXPECT_EQ(
        -1, res.variables["O3_column_number_density_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["scan_subindex"].read_status);
    EXPECT_EQ(-1, res.variables["scan_direction_type"].read_status);
  }

  // 3. Wrong types and incomplete corners / error fields.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ERS_GOME", "GOM_LVL21", 2));

    CodaMemNode* pir = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(pir, "start_orbit", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "pir", pir));

    CodaMemNode* corner0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(corner0, "lat", prod.MakeString("bad")));
    CodaMemNode* glr = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(glr, "datetime", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(glr, "subset_counter", prod.MakeString("bad")));
    ASSERT_TRUE(
        prod.AddField(glr, "corners", MakeSingleNodeArray(prod, corner0)));

    // Provide vcd/total so get_relative_error fails on the second get_data
    // call.
    CodaMemNode* vcd = prod.MakeRecord();
    const double total_vals[] = {300.0, 310.0, 5.0};
    const long three_dims[] = {3};
    ASSERT_TRUE(prod.AddField(vcd, "total",
                              prod.MakeDoubleArray(three_dims, total_vals)));
    CodaMemNode* irr = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(irr, "vcd", vcd));

    CodaMemNode* ddr = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(ddr, "glr", glr));
    ASSERT_TRUE(prod.AddField(ddr, "irr", irr));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "ddr", MakeSingleNodeArray(prod, ddr)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
    EXPECT_EQ(
        -1, res.variables["O3_column_number_density_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["scan_subindex"].read_status);
    EXPECT_EQ(-1, res.variables["scan_direction_type"].read_status);
  }

  // 4. Corners array with 1 valid record (2 fields) fails
  // goto_next_array_element on i=0 -> i=1 in read_latitude_bounds and
  // read_longitude_bounds, plus wrong type in lon (index 1) fails read_double
  // in read_longitude_bounds.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ERS_GOME", "GOM_LVL21", 2));

    CodaMemNode* corner0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(corner0, "lat", prod.MakeDouble(1.0)));
    ASSERT_TRUE(prod.AddField(corner0, "lon", prod.MakeString("bad")));
    CodaMemNode* glr = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(glr, "corners", MakeSingleNodeArray(prod, corner0)));
    CodaMemNode* ddr = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(ddr, "glr", glr));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "ddr", MakeSingleNodeArray(prod, ddr)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
