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
// Unit tests for HIRDLS L2 ingestion module (libharp/harp-ingest-hirdls_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-hirdls_l2.c

#include <cmath>
#include <span>
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

using ::testing::ElementsAre;

constexpr double kMissingVal = -999.0;

void AddMissingValueAttr(CodaMemProduct& prod, CodaMemNode* node,
                         double missing_val = kMissingVal) {
  const long dims[] = {1};
  const double vals[] = {missing_val};
  ASSERT_TRUE(prod.AddAttribute(node, "MissingValue",
                                prod.MakeDoubleArray(dims, vals)));
}

void AddDoubleDataset(CodaMemProduct& prod, CodaMemNode* parent,
                      std::string_view name, std::span<const long> dims,
                      std::span<const double> vals) {
  CodaMemNode* arr = prod.MakeDoubleArray(dims, vals);
  ASSERT_NE(nullptr, arr);
  AddMissingValueAttr(prod, arr);
  ASSERT_TRUE(prod.AddField(parent, name, arr));
}

void BuildHirdlsHierarchy(CodaMemProduct& prod, CodaMemNode** data_fields,
                          CodaMemNode** geo_fields) {
  CodaMemNode* hdfeos = prod.MakeRecord();
  CodaMemNode* swaths = prod.MakeRecord();
  CodaMemNode* hirdls = prod.MakeRecord();
  *data_fields = prod.MakeRecord();
  *geo_fields = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(hirdls, "Data_Fields", *data_fields));
  ASSERT_TRUE(prod.AddField(hirdls, "Geolocation_Fields", *geo_fields));
  ASSERT_TRUE(prod.AddField(swaths, "HIRDLS", hirdls));
  ASSERT_TRUE(prod.AddField(hdfeos, "SWATHS", swaths));
  ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
}

TEST(HarpIngestHirdlsL2Test, IngestFullProductSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("HIRDLS_L2");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  CodaMemNode* data_fields = nullptr;
  CodaMemNode* geo_fields = nullptr;
  BuildHirdlsHierarchy(prod, &data_fields, &geo_fields);

  const long d_time[] = {2};
  const long d_vert[] = {3};
  const long d_2d[] = {2, 3};

  const double tai_base = 220838405.0;  // SECONDS_FROM_1993_TO_2000
  const double time_vals[] = {tai_base + 100.0, tai_base + 200.0};
  const double lon_vals[] = {10.0, kMissingVal};
  const double lat_vals[] = {-20.0, 20.0};
  const double pres_vals[] = {1000.0, 500.0, 100.0};
  const double alt_vals[] = {0.0, 5000.0, 16000.0, 100.0, 5100.0, kMissingVal};

  AddDoubleDataset(prod, geo_fields, "Time", d_time, time_vals);
  AddDoubleDataset(prod, geo_fields, "Longitude", d_time, lon_vals);
  AddDoubleDataset(prod, geo_fields, "Latitude", d_time, lat_vals);
  AddDoubleDataset(prod, geo_fields, "Pressure", d_vert, pres_vals);
  AddDoubleDataset(prod, geo_fields, "Altitude", d_2d, alt_vals);

  struct SpeciesSpec {
    const char* field;
    const char* harp_var;
    double base_val;
  };
  constexpr SpeciesSpec kSpecies[] = {
      {"CFC11", "CCl3F_volume_mixing_ratio", 1e-10},
      {"CFC11Precision", "CCl3F_volume_mixing_ratio_uncertainty", 1e-11},
      {"CFC12", "CCl2F2_volume_mixing_ratio", 2e-10},
      {"CFC12Precision", "CCl2F2_volume_mixing_ratio_uncertainty", 2e-11},
      {"CH4", "CH4_volume_mixing_ratio", 1.8e-6},
      {"CH4Precision", "CH4_volume_mixing_ratio_uncertainty", 1e-7},
      {"ClONO2", "ClONO2_volume_mixing_ratio", 3e-10},
      {"ClONO2Precision", "ClONO2_volume_mixing_ratio_uncertainty", 3e-11},
      {"H2O", "H2O_volume_mixing_ratio", 5e-6},
      {"H2OPrecision", "H2O_volume_mixing_ratio_uncertainty", 5e-7},
      {"HNO3", "HNO3_volume_mixing_ratio", 4e-9},
      {"HNO3Precision", "HNO3_volume_mixing_ratio_uncertainty", 4e-10},
      {"N2O", "N2O_volume_mixing_ratio", 3e-7},
      {"N2OPrecision", "N2O_volume_mixing_ratio_uncertainty", 3e-8},
      {"N2O5", "N2O5_volume_mixing_ratio", 2e-10},
      {"N2O5Precision", "N2O5_volume_mixing_ratio_uncertainty", 2e-11},
      {"NO2", "NO2_volume_mixing_ratio", 1e-9},
      {"NO2Precision", "NO2_volume_mixing_ratio_uncertainty", 1e-10},
      {"O3", "O3_volume_mixing_ratio", 4e-6},
      {"O3Precision", "O3_volume_mixing_ratio_uncertainty", 4e-7},
      {"Temperature", "temperature", 250.0},
      {"TemperaturePrecision", "temperature_uncertainty", 1.5},
  };

  for (const auto& sp : kSpecies) {
    const double vals[] = {sp.base_val,     sp.base_val * 2, sp.base_val * 3,
                           sp.base_val * 4, sp.base_val * 5, kMissingVal};
    AddDoubleDataset(prod, data_fields, sp.field, d_2d, vals);
  }

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(3, res.dimensions[harp_dimension_vertical]);

  EXPECT_THAT(res.variables["datetime"].double_values,
              ElementsAre(100.0, 200.0));
  EXPECT_DOUBLE_EQ(10.0, res.variables["longitude"].double_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["longitude"].double_values[1]));
  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(-20.0, 20.0));
  EXPECT_THAT(res.variables["pressure"].double_values,
              ElementsAre(1000.0, 500.0, 100.0));
  EXPECT_DOUBLE_EQ(0.0, res.variables["altitude"].double_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["altitude"].double_values[5]));

  for (const auto& sp : kSpecies) {
    const auto& vec = res.variables[sp.harp_var].double_values;
    ASSERT_EQ(6u, vec.size());
    EXPECT_DOUBLE_EQ(sp.base_val, vec[0]);
    EXPECT_TRUE(std::isnan(vec[5]));
  }
}

TEST(HarpIngestHirdlsL2Test, ErrorPathsInitCursorsAndDimensions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("HIRDLS_L2");
  ASSERT_NE(nullptr, mod);

  // 1. Missing /HDFEOS/SWATHS/HIRDLS.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Missing Data_Fields.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = prod.MakeRecord();
    CodaMemNode* swaths = prod.MakeRecord();
    CodaMemNode* hirdls = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(swaths, "HIRDLS", hirdls));
    ASSERT_TRUE(prod.AddField(hdfeos, "SWATHS", swaths));
    ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Missing Geolocation_Fields.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = prod.MakeRecord();
    CodaMemNode* swaths = prod.MakeRecord();
    CodaMemNode* hirdls = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(hirdls, "Data_Fields", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(swaths, "HIRDLS", hirdls));
    ASSERT_TRUE(prod.AddField(hdfeos, "SWATHS", swaths));
    ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. Missing Altitude in Geolocation_Fields.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* data_fields = nullptr;
    CodaMemNode* geo_fields = nullptr;
    BuildHirdlsHierarchy(prod, &data_fields, &geo_fields);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 5. Altitude is not an array.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* data_fields = nullptr;
    CodaMemNode* geo_fields = nullptr;
    BuildHirdlsHierarchy(prod, &data_fields, &geo_fields);
    ASSERT_TRUE(prod.AddField(geo_fields, "Altitude", prod.MakeDouble(1.0)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

TEST(HarpIngestHirdlsL2Test, ErrorPathsReadVariableAndAttributes) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("HIRDLS_L2");
  ASSERT_NE(nullptr, mod);

  // Helper building a base hierarchy with 2x3 Altitude and optional Time.
  auto make_base = [](CodaMemProduct& prod, CodaMemNode** data_fields,
                      CodaMemNode** geo_fields, bool add_valid_time = false) {
    BuildHirdlsHierarchy(prod, data_fields, geo_fields);
    const long d_2d[] = {2, 3};
    const double alt[] = {0, 1, 2, 3, 4, 5};
    AddDoubleDataset(prod, *geo_fields, "Altitude", d_2d, alt);
    if (add_valid_time) {
      const long d_time[] = {2};
      const double t[] = {0.0, 1.0};
      AddDoubleDataset(prod, *geo_fields, "Time", d_time, t);
    }
  };

  // 1. Missing Time variable.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* df = nullptr;
    CodaMemNode* gf = nullptr;
    make_base(prod, &df, &gf);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Time is not an array (get_array_dim fails).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* df = nullptr;
    CodaMemNode* gf = nullptr;
    make_base(prod, &df, &gf);
    ASSERT_TRUE(prod.AddField(gf, "Time", prod.MakeDouble(1.0)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Time has wrong num_dimensions (2D instead of 1D).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* df = nullptr;
    CodaMemNode* gf = nullptr;
    make_base(prod, &df, &gf);
    const long d_2d[] = {2, 1};
    const double t[] = {1.0, 2.0};
    AddDoubleDataset(prod, gf, "Time", d_2d, t);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 4. Time has wrong dimension_0 (3 instead of 2).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* df = nullptr;
    CodaMemNode* gf = nullptr;
    make_base(prod, &df, &gf);
    const long d_3[] = {3};
    const double t[] = {1.0, 2.0, 3.0};
    AddDoubleDataset(prod, gf, "Time", d_3, t);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 5. Altitude has wrong dimension_1 (wait: get_dimensions comes from
  // Altitude, so test wrong dimension_1 on CFC11: shape [2, 4] vs [2, 3]).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* df = nullptr;
    CodaMemNode* gf = nullptr;
    make_base(prod, &df, &gf, /*add_valid_time=*/true);
    const long d_time[] = {2};
    const long d_vert[] = {3};
    const double zeros2[] = {0.0, 0.0};
    const double zeros3[] = {0.0, 0.0, 0.0};
    AddDoubleDataset(prod, gf, "Longitude", d_time, zeros2);
    AddDoubleDataset(prod, gf, "Latitude", d_time, zeros2);
    AddDoubleDataset(prod, gf, "Pressure", d_vert, zeros3);
    const long bad_2d[] = {2, 4};
    const std::vector<double> bad_vals(8, 1.0);
    AddDoubleDataset(prod, df, "CFC11", bad_2d, bad_vals);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 6. Time missing attributes / missing MissingValue / scalar MissingValue /
  // string MissingValue / string array Time.
  for (int attr_mode = 0; attr_mode < 4; ++attr_mode) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* df = nullptr;
    CodaMemNode* gf = nullptr;
    make_base(prod, &df, &gf);
    const long d_time[] = {2};
    const double t[] = {1.0, 2.0};
    if (attr_mode == 3) {
      constexpr std::string_view kBadStrs[] = {"a", "b"};
      CodaMemNode* arr = prod.MakeStringArray(d_time, kBadStrs);
      AddMissingValueAttr(prod, arr);
      ASSERT_TRUE(prod.AddField(gf, "Time", arr));
    } else {
      CodaMemNode* arr = prod.MakeDoubleArray(d_time, t);
      if (attr_mode == 1) {
        ASSERT_TRUE(
            prod.AddAttribute(arr, "MissingValue", prod.MakeDouble(-999.0)));
      } else if (attr_mode == 2) {
        const long a_dims[] = {1};
        constexpr std::string_view kBadAttr[] = {"bad"};
        ASSERT_TRUE(prod.AddAttribute(arr, "MissingValue",
                                      prod.MakeStringArray(a_dims, kBadAttr)));
      }
      ASSERT_TRUE(prod.AddField(gf, "Time", arr));
    }
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

}  // namespace
}  // namespace stcorp_harp
