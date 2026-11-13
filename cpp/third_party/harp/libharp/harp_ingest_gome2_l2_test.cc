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
// Unit tests for GOME/GOME-2 Level-2 ingestion modules
// (libharp/harp-ingest-gome2_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-gome2_l2.c

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
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

using ::testing::ElementsAre;
using ::testing::FloatNear;
using ::testing::HasSubstr;

constexpr float kDefaultFillFloat = -999.0f;

// Adds a float array field with a 1-element @FillValue[0] attribute.
bool AddFloatField(CodaMemProduct& prod, CodaMemNode* rec,
                   std::string_view name, std::span<const long> dims,
                   std::span<const float> values,
                   float fill_value = kDefaultFillFloat) {
  CodaMemNode* arr = prod.MakeFloatArray(dims, values);
  if (arr == nullptr) {
    return false;
  }
  constexpr long kOneDim[] = {1};
  const float fill_arr[] = {fill_value};
  if (!prod.AddAttribute(arr, "FillValue",
                         prod.MakeFloatArray(kOneDim, fill_arr))) {
    return false;
  }
  return prod.AddField(rec, name, arr);
}

// Ensures a child record exists under `parent` and returns it.
CodaMemNode* AddChildRecord(CodaMemProduct& prod, CodaMemNode* parent,
                            std::string_view name) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr || !prod.AddField(parent, name, rec)) {
    return nullptr;
  }
  return rec;
}

CodaMemNode* MakeCompoundTimeArray(CodaMemProduct& prod,
                                   std::span<const int32_t> days,
                                   std::span<const int32_t> millis) {
  const long n = static_cast<long>(days.size());
  std::vector<CodaMemNode*> elems;
  elems.reserve(static_cast<size_t>(n));
  for (long i = 0; i < n; ++i) {
    CodaMemNode* rec = prod.MakeRecord();
    if (rec == nullptr || !prod.AddField(rec, "Day", prod.MakeInt32(days[i])) ||
        !prod.AddField(rec, "MillisecondOfDay", prod.MakeInt32(millis[i]))) {
      return nullptr;
    }
    elems.push_back(rec);
  }
  const long dims[] = {n};
  return prod.MakeNodeArray(dims, elems);
}

// Populates common fields for O3MARP / O3MARS aerosol products (num_main = 3).
void PopulateAerosolProduct(CodaMemProduct& prod, bool is_o3mars) {
  constexpr long kN = 3;
  constexpr long kDims1D[] = {kN};
  constexpr long kDimsCorner[] = {4, kN};

  CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
  ASSERT_NE(nullptr, geo);
  CodaMemNode* data = AddChildRecord(prod, prod.root(), "DATA");
  ASSERT_NE(nullptr, data);

  constexpr int32_t kIdx[] = {0, 1, 2};
  ASSERT_TRUE(
      prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kDims1D, kIdx)));

  constexpr std::string_view kTimes[] = {
      "2020-05-15T12:00:00.000",
      "                       ",
      "2020-05-15T12:00:02.500",
  };
  ASSERT_TRUE(
      prod.AddField(geo, "Time", prod.MakeStringArray(kDims1D, kTimes)));

  constexpr float kLon[] = {10.0f, kDefaultFillFloat, 30.0f};
  constexpr float kLat[] = {-20.0f, 0.0f, 20.0f};
  ASSERT_TRUE(AddFloatField(prod, geo, "LongitudeCenter", kDims1D, kLon));
  ASSERT_TRUE(AddFloatField(prod, geo, "LatitudeCenter", kDims1D, kLat));

  // Corner arrays are [4, num_main] before transpose + 2-4-3-1 reorder.
  constexpr float kLonCorner[] = {
      1.0f, 11.0f, 21.0f,  // corner 0
      2.0f, 12.0f, 22.0f,  // corner 1
      3.0f, 13.0f, 23.0f,  // corner 2
      4.0f, 14.0f, 24.0f,  // corner 3
  };
  constexpr float kLatCorner[] = {
      -1.0f, -11.0f, -21.0f,  //
      -2.0f, -12.0f, -22.0f,  //
      -3.0f, -13.0f, -23.0f,  //
      -4.0f, -14.0f, -24.0f,  //
  };
  ASSERT_TRUE(
      AddFloatField(prod, geo, "LongitudeCorner", kDimsCorner, kLonCorner));
  ASSERT_TRUE(
      AddFloatField(prod, geo, "LatitudeCorner", kDimsCorner, kLatCorner));

  constexpr float kAngle[] = {15.0f, 25.0f, 35.0f};
  ASSERT_TRUE(AddFloatField(prod, geo, "SolarAzimuthAngle", kDims1D, kAngle));
  ASSERT_TRUE(AddFloatField(prod, geo, "SolarZenithAngle", kDims1D, kAngle));
  ASSERT_TRUE(
      AddFloatField(prod, geo, "LineOfSightAzimuthAngle", kDims1D, kAngle));
  ASSERT_TRUE(
      AddFloatField(prod, geo, "LineOfSightZenithAngle", kDims1D, kAngle));

  constexpr int32_t kScanDir[] = {1, 2, 0};
  ASSERT_TRUE(prod.AddField(geo, "ScanDirection",
                            prod.MakeInt32Array(kDims1D, kScanDir)));

  constexpr float kAai[] = {1.5f, 2.5f, 3.5f};
  // Use NaN as FillValue on AAI to exercise the !coda_isNaN(fill_value) false
  // branch in read_dataset.
  ASSERT_TRUE(AddFloatField(prod, data, "AAI", kDims1D, kAai,
                            std::numeric_limits<float>::quiet_NaN()));

  constexpr int32_t kGlint[] = {0, 1, 2};
  ASSERT_TRUE(prod.AddField(data, "SunGlintFlag",
                            prod.MakeInt32Array(kDims1D, kGlint)));

  if (is_o3mars) {
    constexpr float kHeight[] = {3.0f, 4.0f, 5.0f};
    constexpr float kHeightErr[] = {0.3f, 0.4f, 0.5f};
    constexpr float kPress[] = {700.0f, 600.0f, 500.0f};
    constexpr float kPressErr[] = {10.0f, 20.0f, 30.0f};
    constexpr int32_t kFlag[] = {0, 1, 2};
    ASSERT_TRUE(AddFloatField(prod, data, "AAH_AbsorbingAerosolHeight", kDims1D,
                              kHeight));
    ASSERT_TRUE(AddFloatField(prod, data, "AAH_AbsorbingAerosolHeightError",
                              kDims1D, kHeightErr));
    ASSERT_TRUE(AddFloatField(prod, data, "AAH_AbsorbingAerosolPressure",
                              kDims1D, kPress));
    ASSERT_TRUE(AddFloatField(prod, data, "AAH_AbsorbingAerosolPressureError",
                              kDims1D, kPressErr));
    ASSERT_TRUE(prod.AddField(data, "AAH_ErrorFlag",
                              prod.MakeInt32Array(kDims1D, kFlag)));
  } else {
    constexpr float kCloud[] = {0.1f, 0.5f, 0.9f};
    ASSERT_TRUE(
        AddFloatField(prod, data, "PMD_CloudFraction", kDims1D, kCloud));
  }
}

// Populates an O3MNHP / O3MOHP ozone profile product (num_main = 2,
// num_vertical = 2, num_state = 3).
void PopulateProfileProduct(CodaMemProduct& prod) {
  constexpr long kN = 2;
  constexpr long kV = 2;
  constexpr long kS = 3;
  constexpr long kDims1D[] = {kN};
  constexpr long kDimsState[] = {kS, kN};
  constexpr long kDimsVertEdge[] = {kV + 1, kN};
  constexpr long kDimsKernel[] = {kS, kS, kN};
  constexpr long kDimsQp[] = {32, kN};

  CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
  ASSERT_NE(nullptr, geo);
  CodaMemNode* data = AddChildRecord(prod, prod.root(), "DATA");
  ASSERT_NE(nullptr, data);

  constexpr std::string_view kTimes[] = {
      "2020-05-15T12:00:00.000",
      "2020-05-15T12:00:01.000",
  };
  ASSERT_TRUE(
      prod.AddField(geo, "Time", prod.MakeStringArray(kDims1D, kTimes)));
  constexpr int32_t kIdx[] = {1, 4};
  ASSERT_TRUE(
      prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kDims1D, kIdx)));

  constexpr float kLon[] = {5.0f, 6.0f};
  constexpr float kLat[] = {45.0f, 46.0f};
  ASSERT_TRUE(AddFloatField(prod, geo, "LongitudeCenter", kDims1D, kLon));
  ASSERT_TRUE(AddFloatField(prod, geo, "LatitudeCenter", kDims1D, kLat));

  constexpr float kA[] = {1.0f, 2.0f};
  constexpr float kB[] = {3.0f, 4.0f};
  constexpr float kC[] = {5.0f, 6.0f};
  constexpr float kD[] = {7.0f, 8.0f};
  ASSERT_TRUE(AddFloatField(prod, geo, "Longitude_A", kDims1D, kA));
  ASSERT_TRUE(AddFloatField(prod, geo, "Longitude_B", kDims1D, kB));
  ASSERT_TRUE(AddFloatField(prod, geo, "Longitude_C", kDims1D, kC));
  ASSERT_TRUE(AddFloatField(prod, geo, "Longitude_D", kDims1D, kD));
  ASSERT_TRUE(AddFloatField(prod, geo, "Latitude_A", kDims1D, kA));
  ASSERT_TRUE(AddFloatField(prod, geo, "Latitude_B", kDims1D, kB));
  ASSERT_TRUE(AddFloatField(prod, geo, "Latitude_C", kDims1D, kC));
  ASSERT_TRUE(AddFloatField(prod, geo, "Latitude_D", kDims1D, kD));

  constexpr float kAng[] = {30.0f, 40.0f};
  ASSERT_TRUE(AddFloatField(prod, geo, "SolarZenithAngle_F", kDims1D, kAng));
  ASSERT_TRUE(AddFloatField(prod, geo, "SolarAzimuthAngle_F", kDims1D, kAng));
  ASSERT_TRUE(
      AddFloatField(prod, geo, "LineOfSightZenithAngle_F", kDims1D, kAng));
  ASSERT_TRUE(
      AddFloatField(prod, geo, "LineOfSightAzimuthAngle_F", kDims1D, kAng));

  constexpr float kStateDef[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  ASSERT_TRUE(AddFloatField(prod, data, "StateDef", kDimsState, kStateDef));
  constexpr float kAlt[] = {0.0f, 1.0f, 10.0f, 11.0f, 20.0f, 21.0f};
  ASSERT_TRUE(
      AddFloatField(prod, data, "AltitudeProfile", kDimsVertEdge, kAlt));
  constexpr float kPressGrid[] = {1000.0f, 990.0f, 500.0f,
                                  490.0f,  100.0f, 90.0f};
  ASSERT_TRUE(AddFloatField(prod, data, "OutputPressureGrid", kDimsVertEdge,
                            kPressGrid));
  constexpr float kTropoP[] = {200.0f, 250.0f};
  ASSERT_TRUE(
      AddFloatField(prod, data, "TropopausePressure", kDims1D, kTropoP));

  // StateRetrieved[num_state, num_main]: rows 0..1 are O3, row 2 is albedo.
  constexpr float kRetrieved[] = {10.0f, 20.0f, 30.0f, 40.0f, 0.25f, 0.35f};
  ASSERT_TRUE(
      AddFloatField(prod, data, "StateRetrieved", kDimsState, kRetrieved));
  constexpr float kApriori[] = {11.0f, 21.0f, 31.0f, 41.0f, 0.2f, 0.3f};
  ASSERT_TRUE(AddFloatField(prod, data, "Apriori", kDimsState, kApriori));

  std::vector<float> kernel(kS * kS * kN);
  for (size_t i = 0; i < kernel.size(); ++i) {
    kernel[i] = static_cast<float>(i + 1) * 0.1f;
  }
  ASSERT_TRUE(
      AddFloatField(prod, data, "AveragingKernel", kDimsKernel, kernel));

  std::vector<int32_t> qp(32 * kN, 2);
  qp[0] = -1;          // sample 0: bit 0 set (< 0)
  qp[1] = 0;           // sample 1: bit 1 set (== 0)
  qp[4 * kN + 0] = 1;  // sample 0: bit 2 set
  qp[5 * kN + 1] = 1;  // sample 1: bit 3 set
  ASSERT_TRUE(prod.AddField(data, "QualityProcessing",
                            prod.MakeInt32Array(kDimsQp, qp)));
}

// Populates a trace gas product (O3MNTO, O3MOTO, ERSNTO, ERSOTO).
// `num_main = 2`, includes all 7 species (plus an unknown "EXTRA" entry),
// plus detailed results and cloud properties for versions 1, 2, or 3.
void PopulateTraceGasProduct(CodaMemProduct& prod, int version,
                             std::string_view revision = "01",
                             bool include_no2_trop_v1 = true,
                             bool include_no2_trop_err_v2 = true,
                             bool compound_time_zero_bounds = false) {
  ASSERT_TRUE(prod.SetProductInfo("GOME2", "O3MOTO", version));

  constexpr long kN = 2;
  constexpr long kW = 8;  // 7 species + 1 unknown
  constexpr long kV = 2;
  constexpr long kDims1D[] = {kN};
  constexpr long kDimsWin[] = {kN, kW};
  constexpr long kDimsVert[] = {kN, kV};
  constexpr long kDimsSo2[] = {kN, 4};
  constexpr long kOneDim[] = {1};

  CodaMemNode* meta = AddChildRecord(prod, prod.root(), "META_DATA");
  ASSERT_NE(nullptr, meta);
  constexpr std::string_view kMainSpecies[] = {
      "BrO", "H2O", "HCHO", "NO2", "O3", "OClO", "SO2", "EXTRA",
  };
  constexpr long kWinDims[] = {kW};
  ASSERT_TRUE(prod.AddField(meta, "MainSpecies",
                            prod.MakeStringArray(kWinDims, kMainSpecies)));
  const std::string_view kContents[] = {"BrO H2O HCHO NO2 O3 OClO SO2"};
  ASSERT_TRUE(prod.AddAttribute(meta, "ProductContents",
                                prod.MakeStringArray(kOneDim, kContents)));
  const std::string_view kRev[] = {revision};
  ASSERT_TRUE(
      prod.AddAttribute(meta, "Revision", prod.MakeStringArray(kOneDim, kRev)));

  CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
  ASSERT_NE(nullptr, geo);

  constexpr int32_t kIdx[] = {0, 3};
  ASSERT_TRUE(
      prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kDims1D, kIdx)));
  constexpr int32_t kSubpix[] = {0, 1};
  ASSERT_TRUE(prod.AddField(geo, "SubpixelInScan",
                            prod.MakeInt32Array(kDims1D, kSubpix)));

  if (compound_time_zero_bounds) {
    constexpr int32_t kDays[] = {0, 0};
    constexpr int32_t kMillis[] = {0, 0};
    ASSERT_TRUE(prod.AddField(geo, "Time",
                              MakeCompoundTimeArray(prod, kDays, kMillis)));
  } else {
    constexpr int32_t kDays[] = {18263, 18264};
    constexpr int32_t kMillis[] = {43200000, 0};
    ASSERT_TRUE(prod.AddField(geo, "Time",
                              MakeCompoundTimeArray(prod, kDays, kMillis)));
  }

  constexpr float kLon[] = {12.0f, 13.0f};
  constexpr float kLat[] = {50.0f, 51.0f};
  ASSERT_TRUE(AddFloatField(prod, geo, "LongitudeCentre", kDims1D, kLon));
  ASSERT_TRUE(AddFloatField(prod, geo, "LatitudeCentre", kDims1D, kLat));
  constexpr float kCorner[] = {1.0f, 2.0f};
  ASSERT_TRUE(AddFloatField(prod, geo, "LongitudeA", kDims1D, kCorner));
  ASSERT_TRUE(AddFloatField(prod, geo, "LongitudeB", kDims1D, kCorner));
  ASSERT_TRUE(AddFloatField(prod, geo, "LongitudeC", kDims1D, kCorner));
  ASSERT_TRUE(AddFloatField(prod, geo, "LongitudeD", kDims1D, kCorner));
  ASSERT_TRUE(AddFloatField(prod, geo, "LatitudeA", kDims1D, kCorner));
  ASSERT_TRUE(AddFloatField(prod, geo, "LatitudeB", kDims1D, kCorner));
  ASSERT_TRUE(AddFloatField(prod, geo, "LatitudeC", kDims1D, kCorner));
  ASSERT_TRUE(AddFloatField(prod, geo, "LatitudeD", kDims1D, kCorner));
  constexpr float kAng[] = {25.0f, 35.0f};
  ASSERT_TRUE(
      AddFloatField(prod, geo, "SolarZenithAngleSatCentre", kDims1D, kAng));
  ASSERT_TRUE(
      AddFloatField(prod, geo, "SolarZenithAngleCentre", kDims1D, kAng));
  ASSERT_TRUE(
      AddFloatField(prod, geo, "LineOfSightZenithAngleCentre", kDims1D, kAng));
  ASSERT_TRUE(AddFloatField(prod, geo, "RelativeAzimuthCentre", kDims1D, kAng));

  CodaMemNode* tot = AddChildRecord(prod, prod.root(), "TOTAL_COLUMNS");
  ASSERT_NE(nullptr, tot);
  constexpr float kCol[] = {100.0f, 200.0f};
  constexpr float kErr[] = {10.0f, 20.0f};
  ASSERT_TRUE(AddFloatField(prod, tot, "BrO", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, tot, "BrO_Error", kDims1D, kErr));
  ASSERT_TRUE(AddFloatField(prod, tot, "H2O", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, tot, "H2O_Error", kDims1D, kErr));
  ASSERT_TRUE(AddFloatField(prod, tot, "HCHO", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, tot, "HCHO_Error", kDims1D, kErr));
  ASSERT_TRUE(AddFloatField(prod, tot, "NO2", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, tot, "NO2_Error", kDims1D, kErr));
  if (version < 2) {
    if (include_no2_trop_v1) {
      ASSERT_TRUE(AddFloatField(prod, tot, "NO2_Trop", kDims1D, kCol));
    }
  } else {
    ASSERT_TRUE(AddFloatField(prod, tot, "NO2Tropo", kDims1D, kCol));
    if (include_no2_trop_err_v2) {
      ASSERT_TRUE(AddFloatField(prod, tot, "NO2Tropo_Error", kDims1D, kErr));
    }
  }
  ASSERT_TRUE(AddFloatField(prod, tot, "O3", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, tot, "O3_Error", kDims1D, kErr));
  ASSERT_TRUE(AddFloatField(prod, tot, "OClO", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, tot, "OClO_Error", kDims1D, kErr));
  ASSERT_TRUE(AddFloatField(prod, tot, "SO2", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, tot, "SO2_Error", kDims1D, kErr));

  CodaMemNode* det = AddChildRecord(prod, prod.root(), "DETAILED_RESULTS");
  ASSERT_NE(nullptr, det);

  std::vector<float> win_vals(kN * kW);
  std::vector<int32_t> qf_vals(kN * kW);
  for (size_t i = 0; i < win_vals.size(); ++i) {
    win_vals[i] = static_cast<float>(i + 1);
    qf_vals[i] = static_cast<int32_t>(i % 7);
  }
  ASSERT_TRUE(AddFloatField(prod, det, "AMFTotal", kDimsWin, win_vals));
  ASSERT_TRUE(AddFloatField(prod, det, "AMFTotal_Error", kDimsWin, win_vals));
  ASSERT_TRUE(AddFloatField(prod, det, "ESC", kDimsWin, win_vals));
  ASSERT_TRUE(AddFloatField(prod, det, "ESC_Error", kDimsWin, win_vals));
  ASSERT_TRUE(AddFloatField(prod, det, "SurfaceAlbedo", kDimsWin, win_vals));
  ASSERT_TRUE(prod.AddField(det, "QualityFlags",
                            prod.MakeInt32Array(kDimsWin, qf_vals)));
  ASSERT_TRUE(AddFloatField(prod, det, "AAI", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, det, "SurfaceHeight", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, det, "SurfacePressure", kDims1D, kCol));

  constexpr int32_t kFlag1D[] = {1, 2};
  constexpr float kProfile2D[] = {1000.0f, 500.0f, 900.0f, 400.0f};

  // BrO detailed
  CodaMemNode* bro = AddChildRecord(prod, det, "BrO");
  ASSERT_NE(nullptr, bro);
  ASSERT_TRUE(AddFloatField(prod, bro, "VCDCorrected", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, bro, "VCDCorrected_Error", kDims1D, kErr));
  ASSERT_TRUE(AddFloatField(prod, bro, "ESCCorrected", kDims1D, kCol));

  // H2O detailed
  CodaMemNode* h2o = AddChildRecord(prod, det, "H2O");
  ASSERT_NE(nullptr, h2o);
  ASSERT_TRUE(
      prod.AddField(h2o, "H2O_Flag", prod.MakeInt32Array(kDims1D, kFlag1D)));
  ASSERT_TRUE(AddFloatField(prod, h2o, "VCDCorrected", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, h2o, "VCDCorrected_Error", kDims1D, kErr));
  ASSERT_TRUE(AddFloatField(prod, h2o, "ESCCorrected", kDims1D, kCol));

  // HCHO detailed
  CodaMemNode* hcho = AddChildRecord(prod, det, "HCHO");
  ASSERT_NE(nullptr, hcho);
  ASSERT_TRUE(
      prod.AddField(hcho, "HCHO_Flag", prod.MakeInt32Array(kDims1D, kFlag1D)));
  ASSERT_TRUE(AddFloatField(prod, hcho, "VCDCorrected", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, hcho, "VCDCorrected_Error", kDims1D, kErr));
  ASSERT_TRUE(AddFloatField(prod, hcho, "ESCCorrected", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, hcho, "AveragingKernelPressureLevel",
                            kDimsVert, kProfile2D));
  ASSERT_TRUE(
      AddFloatField(prod, hcho, "AprioriHCHOProfile", kDimsVert, kProfile2D));
  ASSERT_TRUE(
      AddFloatField(prod, hcho, "AveragingKernel", kDimsVert, kProfile2D));

  // NO2 detailed
  CodaMemNode* no2 = AddChildRecord(prod, det, "NO2");
  ASSERT_NE(nullptr, no2);
  ASSERT_TRUE(AddFloatField(prod, no2, "VCDCorrected", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, no2, "VCDCorrected_Error", kDims1D, kErr));
  ASSERT_TRUE(AddFloatField(prod, no2, "VCDTropo", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, no2, "VCDTropo_Error", kDims1D, kErr));
  ASSERT_TRUE(prod.AddField(no2, "NO2Tropo_Flag",
                            prod.MakeInt32Array(kDims1D, kFlag1D)));
  ASSERT_TRUE(AddFloatField(prod, no2, "AMFTropo", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, no2, "AMFTropo_Error", kDims1D, kErr));
  ASSERT_TRUE(AddFloatField(prod, no2, "AveragingKernelPressureLevel",
                            kDimsVert, kProfile2D));
  ASSERT_TRUE(
      AddFloatField(prod, no2, "AprioriNO2Profile", kDimsVert, kProfile2D));
  ASSERT_TRUE(
      AddFloatField(prod, no2, "AveragingKernel", kDimsVert, kProfile2D));

  // O3 detailed
  CodaMemNode* o3 = AddChildRecord(prod, det, "O3");
  ASSERT_NE(nullptr, o3);
  ASSERT_TRUE(AddFloatField(prod, o3, "VCDCorrected", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, o3, "VCDCorrected_Error", kDims1D, kErr));
  ASSERT_TRUE(AddFloatField(prod, o3, "O3Temperature", kDims1D, kCol));
  ASSERT_TRUE(prod.AddField(o3, "O3_Volcano_Flag",
                            prod.MakeInt32Array(kDims1D, kFlag1D)));

  // OClO detailed
  CodaMemNode* oclo = AddChildRecord(prod, det, "OClO");
  ASSERT_NE(nullptr, oclo);
  ASSERT_TRUE(
      prod.AddField(oclo, "OClO_Flag", prod.MakeInt32Array(kDims1D, kFlag1D)));
  ASSERT_TRUE(AddFloatField(prod, oclo, "ESCCorrected", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, oclo, "ESCCorrected_Error", kDims1D, kErr));

  // SO2 detailed
  CodaMemNode* so2 = AddChildRecord(prod, det, "SO2");
  ASSERT_NE(nullptr, so2);
  ASSERT_TRUE(
      prod.AddField(so2, "SO2_Flag", prod.MakeInt32Array(kDims1D, kFlag1D)));
  constexpr int32_t kSo2Volc[] = {0, 2};
  ASSERT_TRUE(prod.AddField(so2, "SO2_Volcano_Flag",
                            prod.MakeInt32Array(kDims1D, kSo2Volc)));
  constexpr float kSo2Mat[] = {
      1.0f, 2.0f, 3.0f, kDefaultFillFloat, 5.0f, 6.0f, 7.0f, 8.0f,
  };
  ASSERT_TRUE(AddFloatField(prod, so2, "VCDCorrected", kDimsSo2, kSo2Mat));
  ASSERT_TRUE(
      AddFloatField(prod, so2, "VCDCorrected_Error", kDimsSo2, kSo2Mat));
  ASSERT_TRUE(AddFloatField(prod, so2, "AMFTotal", kDimsSo2, kSo2Mat));
  ASSERT_TRUE(AddFloatField(prod, so2, "AMFTotal_Error", kDimsSo2, kSo2Mat));
  ASSERT_TRUE(AddFloatField(prod, so2, "ESCCorrected", kDimsSo2, kSo2Mat));

  // Cloud properties live in DETAILED_RESULTS for v1 and CLOUD_PROPERTIES for
  // v2+.
  CodaMemNode* cloud_parent =
      (version < 2) ? det
                    : AddChildRecord(prod, prod.root(), "CLOUD_PROPERTIES");
  ASSERT_NE(nullptr, cloud_parent);
  ASSERT_TRUE(
      AddFloatField(prod, cloud_parent, "CloudFraction", kDims1D, kCol));
  ASSERT_TRUE(
      AddFloatField(prod, cloud_parent, "CloudFraction_Error", kDims1D, kErr));
  ASSERT_TRUE(
      AddFloatField(prod, cloud_parent, "CloudTopPressure", kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, cloud_parent, "CloudTopPressure_Error",
                            kDims1D, kErr));
  ASSERT_TRUE(
      AddFloatField(prod, cloud_parent, "CloudTopHeight", kDims1D, kCol));
  ASSERT_TRUE(
      AddFloatField(prod, cloud_parent, "CloudTopHeight_Error", kDims1D, kErr));
  ASSERT_TRUE(
      AddFloatField(prod, cloud_parent, "CloudTopAlbedo", kDims1D, kCol));
  ASSERT_TRUE(
      AddFloatField(prod, cloud_parent, "CloudTopAlbedo_Error", kDims1D, kErr));
  ASSERT_TRUE(AddFloatField(prod, cloud_parent, "CloudOpticalThickness",
                            kDims1D, kCol));
  ASSERT_TRUE(AddFloatField(prod, cloud_parent, "CloudOpticalThickness_Error",
                            kDims1D, kErr));
}

// Same as ExerciseIngestionModule, but skips `skip_var` (used for O3MNHP/O3MOHP
// because upstream `ingestion_init_profile` leaves `index_in_scan_buffer.ptr`
// uninitialized and `ingestion_done_profile` does not free it).
IngestExerciseResult ExerciseModuleSkippingVar(const harp_ingestion_module* mod,
                                               coda_product* cprod,
                                               std::string_view skip_var,
                                               bool stop_on_error) {
  IngestExerciseResult result;
  harp_ingestion_options* raw_opts = nullptr;
  if (harp_ingestion_options_new(&raw_opts) != 0) {
    result.failed_errno = harp_errno;
    return result;
  }
  ScopedHarpIngestionOptions opts(raw_opts);

  harp_product_definition* prod_def = nullptr;
  void* user_data = nullptr;
  if (mod->ingestion_init(mod, cprod, opts.get(), &prod_def, &user_data) != 0) {
    result.failed_errno = harp_errno;
    return result;
  }
  auto done_guard = absl::MakeCleanup([&] {
    if (mod->ingestion_done != nullptr && user_data != nullptr) {
      mod->ingestion_done(user_data);
    }
  });

  if (prod_def == nullptr || prod_def->read_dimensions == nullptr ||
      prod_def->read_dimensions(user_data, result.dimensions) != 0) {
    result.failed_errno = harp_errno;
    return result;
  }

  result.status = HARP_SUCCESS;
  for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
    const harp_variable_definition* vdef = prod_def->variable_definition[i];
    if (std::string_view(vdef->name) == skip_var) {
      continue;
    }
    IngestedVariable var;
    var.data_type = vdef->data_type;
    if (vdef->include != nullptr && vdef->include(user_data) == 0) {
      var.included = false;
      result.variables.emplace(vdef->name, std::move(var));
      continue;
    }

    long num_elements = 1;
    long dim0 = 1;
    long stride = 1;
    for (int d = 0; d < vdef->num_dimensions; ++d) {
      const long len = (vdef->dimension_type[d] == harp_dimension_independent)
                           ? vdef->dimension[d]
                           : result.dimensions[vdef->dimension_type[d]];
      if (d == 0) {
        dim0 = len;
      } else {
        stride *= len;
      }
      num_elements *= len;
    }
    var.num_elements = num_elements;
    if (num_elements == 0) {
      result.variables.emplace(vdef->name, std::move(var));
      continue;
    }

    const size_t elem_size =
        static_cast<size_t>(harp_get_size_for_type(vdef->data_type));
    std::vector<uint8_t> raw(static_cast<size_t>(num_elements) * elem_size, 0);
    harp_array data{};
    data.ptr = raw.data();
    int rc = 0;
    if (vdef->read_all != nullptr) {
      rc = vdef->read_all(user_data, data);
    } else if (vdef->read_block != nullptr) {
      for (long blk = 0; blk < dim0 && rc == 0; ++blk) {
        harp_array slice{};
        slice.ptr = raw.data() + static_cast<size_t>(blk * stride) * elem_size;
        rc = vdef->read_block(user_data, blk, slice);
      }
    }
    var.read_status = rc;
    if (rc != 0) {
      if (result.status == HARP_SUCCESS) {
        result.status = rc;
        result.failed_errno = harp_errno;
        result.failed_variable = vdef->name;
      }
      result.variables.emplace(vdef->name, std::move(var));
      if (stop_on_error) {
        return result;
      }
      continue;
    }
    switch (vdef->data_type) {
      case harp_type_int8:
        var.int8_values.assign(
            reinterpret_cast<const int8_t*>(raw.data()),
            reinterpret_cast<const int8_t*>(raw.data()) + num_elements);
        break;
      case harp_type_float:
        var.float_values.assign(
            reinterpret_cast<const float*>(raw.data()),
            reinterpret_cast<const float*>(raw.data()) + num_elements);
        break;
      case harp_type_double:
        var.double_values.assign(
            reinterpret_cast<const double*>(raw.data()),
            reinterpret_cast<const double*>(raw.data()) + num_elements);
        break;
      default:
        break;
    }
    result.variables.emplace(vdef->name, std::move(var));
  }
  return result;
}

TEST(HarpIngestGome2L2Test, IngestO3marpAndO3marsSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // Upstream bug in libharp/harp-ingest-gome2_l2.c (`ingestion_init_aerosol`):
  // `info->num_vertical` is left uninitialized before `read_dimensions` checks
  // `if (info->num_vertical > 0)`.
#if !defined(MEMORY_SANITIZER)
  {
    const harp_ingestion_module* mod = FindIngestionModule("GOME2_L2_O3MARP");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateAerosolProduct(prod, /*is_o3mars=*/false);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(3, res.dimensions[harp_dimension_time]);
    EXPECT_TRUE(res.has_datetime_range);
    EXPECT_LT(res.datetime_start, res.datetime_stop);
    EXPECT_TRUE(std::isnan(res.variables["datetime"].double_values[1]));
    EXPECT_TRUE(std::isnan(res.variables["longitude"].float_values[1]));
    EXPECT_THAT(res.variables["scan_direction_type"].int8_values,
                ElementsAre(0, 1, -1));
    EXPECT_THAT(res.variables["cloud_fraction"].float_values,
                ElementsAre(0.1f, 0.5f, 0.9f));
    // First pixel corners: original [1, 2, 3, 4] -> 2-4-3-1 => [2, 4, 3, 1].
    EXPECT_EQ(2.0f, res.variables["longitude_bounds"].float_values[0]);
    EXPECT_EQ(4.0f, res.variables["longitude_bounds"].float_values[1]);
    EXPECT_EQ(3.0f, res.variables["longitude_bounds"].float_values[2]);
    EXPECT_EQ(1.0f, res.variables["longitude_bounds"].float_values[3]);
  }

  {
    const harp_ingestion_module* mod = FindIngestionModule("GOME2_L2_O3MARS");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateAerosolProduct(prod, /*is_o3mars=*/true);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["aerosol_height"].float_values,
                ElementsAre(3.0f, 4.0f, 5.0f));
    EXPECT_THAT(res.variables["aerosol_height_uncertainty"].float_values,
                ElementsAre(0.3f, 0.4f, 0.5f));
    EXPECT_THAT(res.variables["aerosol_pressure"].float_values,
                ElementsAre(700.0f, 600.0f, 500.0f));
    EXPECT_THAT(res.variables["aerosol_pressure_uncertainty"].float_values,
                ElementsAre(10.0f, 20.0f, 30.0f));
    EXPECT_THAT(res.variables["validity"].int32_values, ElementsAre(0, 1, 2));
  }
#endif  // !defined(MEMORY_SANITIZER)
}

TEST(HarpIngestGome2L2Test, IngestOzoneProfileProductsSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr std::string_view kProfileModules[] = {
      "GOME2_L2_O3MNHP",
      "GOME2_L2_O3MOHP",
  };
  for (std::string_view mod_name : kProfileModules) {
    const harp_ingestion_module* mod = FindIngestionModule(mod_name);
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateProfileProduct(prod);

    IngestExerciseResult res = ExerciseModuleSkippingVar(
        mod, prod.product(), "scan_subindex", /*stop_on_error=*/true);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2, res.dimensions[harp_dimension_vertical]);
    EXPECT_THAT(res.variables["scene_albedo"].float_values,
                ElementsAre(0.25f, 0.35f));
    EXPECT_THAT(res.variables["tropopause_pressure"].float_values,
                ElementsAre(200.0f, 250.0f));
    // Sample 0: bit 0 (<0) + bit 2 (col4==1) = 5; Sample 1: bit 1 (==0) +
    // bit 3 (col5==1) = 10.
    EXPECT_THAT(res.variables["validity"].int8_values, ElementsAre(5, 10));
    EXPECT_THAT(res.variables["O3_column_number_density"].float_values,
                ElementsAre(10.0f, 30.0f, 20.0f, 40.0f));
    EXPECT_THAT(res.variables["O3_column_number_density_apriori"].float_values,
                ElementsAre(11.0f, 31.0f, 21.0f, 41.0f));
    EXPECT_EQ(8u, res.variables["altitude_bounds"].float_values.size());
    EXPECT_EQ(8u, res.variables["pressure_bounds"].float_values.size());
    EXPECT_EQ(
        8u, res.variables["O3_column_number_density_avk"].float_values.size());
    EXPECT_EQ(8u, res.variables["O3_column_number_density_covariance"]
                      .float_values.size());
  }
}

TEST(HarpIngestGome2L2Test, IngestTraceGasesVersionsAndOptions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // 1. Version 1 on O3MNTO (revision="00", zero compound time bounds, missing
  // NO2_Trop variant also checked).
  {
    const harp_ingestion_module* mod = FindIngestionModule("GOME2_L2_O3MNTO");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateTraceGasProduct(prod, /*version=*/1, /*revision=*/"00",
                            /*include_no2_trop_v1=*/true,
                            /*include_no2_trop_err_v2=*/false,
                            /*compound_time_zero_bounds=*/true);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_TRUE(std::isinf(res.datetime_start) && res.datetime_start < 0);
    EXPECT_TRUE(std::isinf(res.datetime_stop) && res.datetime_stop > 0);
    EXPECT_TRUE(std::isnan(res.variables["datetime"].double_values[0]));
    // revision=00 shifts subpixel via (val + 31) % 32 -> (0+31)%32=31,
    // (1+31)%32=0.
    EXPECT_THAT(res.variables["scan_subindex"].int8_values, ElementsAre(31, 0));
    EXPECT_THAT(res.variables["scan_direction_type"].int8_values,
                ElementsAre(0, 1));
    EXPECT_TRUE(
        res.variables["tropospheric_NO2_column_number_density"].included);
    EXPECT_FALSE(
        res.variables["tropospheric_NO2_column_number_density_uncertainty"]
            .included);
    EXPECT_FALSE(res.variables["O3_effective_temperature"].included);
    EXPECT_FALSE(res.variables["surface_albedo"].included);
  }

  // 2. Version 1 on ERSNTO with NO2_Trop absent and ERSOTO with default
  // options.
  {
    const harp_ingestion_module* mod = FindIngestionModule("GOME_L2_ERSNTO");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateTraceGasProduct(prod, /*version=*/1, /*revision=*/"01",
                            /*include_no2_trop_v1=*/false);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(
        res.variables["tropospheric_NO2_column_number_density"].included);
    EXPECT_THAT(res.variables["scan_subindex"].int8_values, ElementsAre(0, 3));
  }
  {
    const harp_ingestion_module* mod = FindIngestionModule("GOME_L2_ERSOTO");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateTraceGasProduct(prod, /*version=*/2, /*revision=*/"01",
                            /*include_no2_trop_v1=*/false,
                            /*include_no2_trop_err_v2=*/false);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_TRUE(
        res.variables["tropospheric_NO2_column_number_density"].included);
    EXPECT_FALSE(
        res.variables["tropospheric_NO2_column_number_density_uncertainty"]
            .included);
  }

  // 3. Version 2 on O3MOTO with detailed_results=NO2, O3, and HCHO+corrected.
  {
    const harp_ingestion_module* mod = FindIngestionModule("GOME2_L2_O3MOTO");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateTraceGasProduct(prod, /*version=*/2);

    IngestExerciseResult res_no2 =
        ExerciseIngestionModule(mod, prod.product(), "detailed_results=NO2");
    ASSERT_EQ(HARP_SUCCESS, res_no2.status);
    EXPECT_TRUE(res_no2.variables["tropospheric_NO2_column_number_density_amf"]
                    .included);
    EXPECT_TRUE(
        res_no2
            .variables["tropospheric_NO2_column_number_density_amf_uncertainty"]
            .included);
    EXPECT_TRUE(
        res_no2.variables["tropospheric_NO2_column_number_density_validity"]
            .included);
    EXPECT_FALSE(res_no2.variables["pressure"].included);

    IngestExerciseResult res_o3 =
        ExerciseIngestionModule(mod, prod.product(), "detailed_results=O3");
    ASSERT_EQ(HARP_SUCCESS, res_o3.status);
    EXPECT_TRUE(res_o3.variables["O3_effective_temperature"].included);

    IngestExerciseResult res_hcho = ExerciseIngestionModule(
        mod, prod.product(), "detailed_results=HCHO;corrected=true");
    ASSERT_EQ(HARP_SUCCESS, res_hcho.status);
    EXPECT_FALSE(
        res_hcho.variables["HCHO_column_number_density_uncertainty"].included);
  }

  // 4. Version 3 on O3MOTO exercising all 7 species (corrected and uncorrected)
  // plus SO2 plume height variants.
  {
    const harp_ingestion_module* mod = FindIngestionModule("GOME2_L2_O3MOTO");
    ASSERT_NE(nullptr, mod);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateTraceGasProduct(prod, /*version=*/3);

    constexpr const char* kOptionCases[] = {
        nullptr,
        "detailed_results=BrO",
        "detailed_results=BrO;corrected=true",
        "detailed_results=H2O",
        "detailed_results=H2O;corrected=true",
        "detailed_results=HCHO",
        "detailed_results=HCHO;corrected=true",
        "detailed_results=NO2",
        "detailed_results=NO2;corrected=true",
        "detailed_results=O3",
        "detailed_results=O3;corrected=true",
        "detailed_results=OClO",
        "detailed_results=OClO;corrected=true",
        "detailed_results=SO2",
        "detailed_results=SO2;corrected=true",
        "detailed_results=SO2;so2_column=15km",
        "detailed_results=SO2;so2_column=2.5km",
        "detailed_results=SO2;so2_column=1km",
    };
    for (const char* opts : kOptionCases) {
      IngestExerciseResult res =
          ExerciseIngestionModule(mod, prod.product(), opts);
      ASSERT_EQ(HARP_SUCCESS, res.status)
          << "failed with options=" << (opts ? opts : "<null>");
    }

    IngestExerciseResult res_no2 = ExerciseIngestionModule(
        mod, prod.product(), "detailed_results=NO2;corrected=true");
    ASSERT_EQ(HARP_SUCCESS, res_no2.status);
    EXPECT_EQ(2, res_no2.dimensions[harp_dimension_vertical]);
    EXPECT_TRUE(res_no2.variables["pressure"].included);
    EXPECT_TRUE(
        res_no2.variables["NO2_volume_mixing_ratio_dry_air_apriori"].included);
    EXPECT_TRUE(res_no2.variables["NO2_column_number_density_avk"].included);
    EXPECT_TRUE(res_no2.variables["surface_albedo"].included);
    // AveragingKernelPressureLevel was [1000, 500, 900, 400]; inverted along
    // vertical -> [500, 1000, 400, 900].
    EXPECT_THAT(res_no2.variables["pressure"].float_values,
                ElementsAre(500.0f, 1000.0f, 400.0f, 900.0f));
    EXPECT_THAT(res_no2.variables["O3_column_number_density"].float_values[0],
                FloatNear(2.68708e18f, 1e14f));
  }
}

TEST(HarpIngestGome2L2Test, TraceGasOptionAndWindowValidationErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  const harp_ingestion_module* mod = FindIngestionModule("GOME2_L2_O3MOTO");
  ASSERT_NE(nullptr, mod);

  // 1. Species present in MainSpecies but missing from ProductContents are
  // excluded, so detailed_results=<species> fails for all 7 species.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateTraceGasProduct(prod, /*version=*/3);
    // Build a second minimal product where ProductContents only lists "NONE".
    CodaMemProduct empty_contents;
    ASSERT_TRUE(empty_contents.ok());
    ASSERT_TRUE(empty_contents.SetProductInfo("GOME2", "O3MOTO", 3));
    CodaMemNode* meta =
        AddChildRecord(empty_contents, empty_contents.root(), "META_DATA");
    CodaMemNode* geo =
        AddChildRecord(empty_contents, empty_contents.root(), "GEOLOCATION");
    constexpr long kOne[] = {1};
    constexpr int32_t kIdx[] = {0};
    ASSERT_TRUE(empty_contents.AddField(
        geo, "IndexInScan", empty_contents.MakeInt32Array(kOne, kIdx)));
    constexpr std::string_view kSp[] = {
        "BrO", "H2O", "HCHO", "NO2", "O3", "OClO", "SO2",
    };
    constexpr long kSpDim[] = {7};
    ASSERT_TRUE(empty_contents.AddField(
        meta, "MainSpecies", empty_contents.MakeStringArray(kSpDim, kSp)));
    constexpr std::string_view kNone[] = {"NONE"};
    ASSERT_TRUE(empty_contents.AddAttribute(
        meta, "ProductContents", empty_contents.MakeStringArray(kOne, kNone)));
    constexpr std::string_view kRev[] = {"01"};
    ASSERT_TRUE(empty_contents.AddAttribute(
        meta, "Revision", empty_contents.MakeStringArray(kOne, kRev)));

    constexpr const char* kSpeciesOpts[] = {
        "detailed_results=BrO", "detailed_results=H2O", "detailed_results=HCHO",
        "detailed_results=NO2", "detailed_results=O3",  "detailed_results=OClO",
        "detailed_results=SO2",
    };
    for (const char* opts : kSpeciesOpts) {
      IngestExerciseResult res =
          ExerciseIngestionModule(mod, empty_contents.product(), opts);
      EXPECT_EQ(-1, res.status);
      EXPECT_EQ(HARP_ERROR_INVALID_INGESTION_OPTION_VALUE, res.failed_errno);
    }
  }

  // 2. Invalid option combinations against version 1 and version 2 products.
  {
    CodaMemProduct prod_v1;
    ASSERT_TRUE(prod_v1.ok());
    PopulateTraceGasProduct(prod_v1, /*version=*/1);
    constexpr const char* kBadOptsV1[] = {
        "corrected=true",
        "so2_column=15km",
        "detailed_results=NO2;so2_column=15km",
        "detailed_results=BrO;corrected=true",
        "detailed_results=H2O;corrected=true",
        "detailed_results=HCHO;corrected=true",
        "detailed_results=NO2;corrected=true",
        "detailed_results=O3;corrected=true",
        "detailed_results=SO2;so2_column=1km",
    };
    for (const char* opts : kBadOptsV1) {
      IngestExerciseResult res =
          ExerciseIngestionModule(mod, prod_v1.product(), opts);
      EXPECT_EQ(-1, res.status) << "expected failure for " << opts;
      EXPECT_EQ(HARP_ERROR_INVALID_INGESTION_OPTION_VALUE, res.failed_errno);
    }
  }
}

TEST(HarpIngestGome2L2Test, InitAndReaderErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  const harp_ingestion_module* mod_arp = FindIngestionModule("GOME2_L2_O3MARP");
  const harp_ingestion_module* mod_prof =
      FindIngestionModule("GOME2_L2_O3MNHP");
  const harp_ingestion_module* mod_tg = FindIngestionModule("GOME2_L2_O3MOTO");
  ASSERT_NE(nullptr, mod_arp);
  ASSERT_NE(nullptr, mod_prof);
  ASSERT_NE(nullptr, mod_tg);

  // 1. Missing IndexInScan in aerosol init.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod_arp, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 2. Aerosol datetime_range & variable read errors (missing Time, invalid
  // time string, wrong element count, missing @FillValue[0], wrong type).
  // Guarded under MSan because `ingestion_init_aerosol` leaves
  // `info->num_vertical` uninitialized before `read_dimensions` reads it.
#if !defined(MEMORY_SANITIZER)
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    constexpr long kOne[] = {1};
    constexpr int32_t kIdx[] = {0};
    ASSERT_TRUE(
        prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx)));
    // Missing GEOLOCATION/Time -> read_datetime_range_string fails.
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_arp, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    constexpr long kOne[] = {1};
    constexpr int32_t kIdx[] = {0};
    constexpr std::string_view kBadTime[] = {"not-a-timestamp"};
    ASSERT_TRUE(
        prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx)));
    ASSERT_TRUE(
        prod.AddField(geo, "Time", prod.MakeStringArray(kOne, kBadTime)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_arp, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    constexpr long kOne[] = {1};
    constexpr long kTwo[] = {2};
    constexpr int32_t kIdx[] = {0};
    constexpr std::string_view kGoodTime[] = {"2020-05-15T12:00:00.000"};
    constexpr float kTwoFloats[] = {1.0f, 2.0f};
    constexpr float kOneFloat[] = {1.0f};
    ASSERT_TRUE(
        prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx)));
    ASSERT_TRUE(
        prod.AddField(geo, "Time", prod.MakeStringArray(kOne, kGoodTime)));
    // LongitudeCenter wrong element count (2 vs 1); LatitudeCenter missing
    // @FillValue[0]; SolarAzimuthAngle wrong type (string); ScanDirection wrong
    // type.
    ASSERT_TRUE(AddFloatField(prod, geo, "LongitudeCenter", kTwo, kTwoFloats));
    ASSERT_TRUE(prod.AddField(geo, "LatitudeCenter",
                              prod.MakeFloatArray(kOne, kOneFloat)));
    constexpr std::string_view kBadStr[] = {"bad"};
    ASSERT_TRUE(prod.AddField(geo, "SolarAzimuthAngle",
                              prod.MakeStringArray(kOne, kBadStr)));
    ASSERT_TRUE(prod.AddField(geo, "ScanDirection",
                              prod.MakeStringArray(kOne, kBadStr)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod_arp, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["solar_azimuth_angle"].read_status);
    EXPECT_EQ(-1, res.variables["scan_direction_type"].read_status);
  }
#endif  // !defined(MEMORY_SANITIZER)

  // 3. Profile init errors (missing Time, missing StateDef, missing
  // AltitudeProfile).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_prof, prod.product()).status);

    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    constexpr long kOne[] = {1};
    constexpr std::string_view kGoodTime[] = {"2020-05-15T12:00:00.000"};
    ASSERT_TRUE(
        prod.AddField(geo, "Time", prod.MakeStringArray(kOne, kGoodTime)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_prof, prod.product()).status);

    CodaMemNode* data = AddChildRecord(prod, prod.root(), "DATA");
    constexpr float kDef[] = {1.0f, 2.0f};
    constexpr long kTwo[] = {2};
    ASSERT_TRUE(AddFloatField(prod, data, "StateDef", kTwo, kDef));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_prof, prod.product()).status);
  }

  // 4. Trace gas init errors: MainSpecies wrong dims, 0-window MainSpecies,
  // missing ProductContents, missing Revision, invalid Revision,
  // init_num_vertical wrong dims / wrong dim[0].
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GOME2", "O3MOTO", 3));
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    CodaMemNode* meta = AddChildRecord(prod, prod.root(), "META_DATA");
    constexpr long kOne[] = {1};
    constexpr int32_t kIdx[] = {0};
    ASSERT_TRUE(
        prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx)));
    // Missing MainSpecies.
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod.product()).status);
    (void)meta;
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GOME2", "O3MOTO", 3));
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    CodaMemNode* meta = AddChildRecord(prod, prod.root(), "META_DATA");
    constexpr long kOne[] = {1};
    constexpr int32_t kIdx[] = {0};
    ASSERT_TRUE(
        prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx)));
    constexpr long kDims2D[] = {1, 1};
    constexpr std::string_view kSp[] = {"O3"};
    ASSERT_TRUE(
        prod.AddField(meta, "MainSpecies", prod.MakeStringArray(kDims2D, kSp)));
    IngestExerciseResult res = ExerciseIngestionModule(mod_tg, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    // 0-element MainSpecies + bad Revision.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GOME2", "O3MOTO", 3));
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    CodaMemNode* meta = AddChildRecord(prod, prod.root(), "META_DATA");
    constexpr long kOne[] = {1};
    constexpr int32_t kIdx[] = {0};
    ASSERT_TRUE(
        prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx)));
    constexpr long kZero[] = {0};
    ASSERT_TRUE(prod.AddField(
        meta, "MainSpecies",
        prod.MakeStringArray(kZero, std::span<const std::string_view>{})));
    constexpr std::string_view kCont[] = {""};
    ASSERT_TRUE(prod.AddAttribute(meta, "ProductContents",
                                  prod.MakeStringArray(kOne, kCont)));
    constexpr std::string_view kBadRev[] = {"XY"};
    ASSERT_TRUE(prod.AddAttribute(meta, "Revision",
                                  prod.MakeStringArray(kOne, kBadRev)));
    IngestExerciseResult res = ExerciseIngestionModule(mod_tg, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    // init_num_vertical failure: 1D AveragingKernelPressureLevel and mismatched
    // dim[0].
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateTraceGasProduct(prod, /*version=*/3);
    // Replace AveragingKernelPressureLevel on HCHO with 1D array, and on NO2
    // with wrong dim[0].
    CodaMemProduct prod1d;
    ASSERT_TRUE(prod1d.ok());
    ASSERT_TRUE(prod1d.SetProductInfo("GOME2", "O3MOTO", 3));
    CodaMemNode* meta = AddChildRecord(prod1d, prod1d.root(), "META_DATA");
    CodaMemNode* geo = AddChildRecord(prod1d, prod1d.root(), "GEOLOCATION");
    CodaMemNode* det =
        AddChildRecord(prod1d, prod1d.root(), "DETAILED_RESULTS");
    CodaMemNode* hcho = AddChildRecord(prod1d, det, "HCHO");
    CodaMemNode* no2 = AddChildRecord(prod1d, det, "NO2");
    constexpr long kOne[] = {1};
    constexpr long kSpDim[] = {2};
    constexpr int32_t kIdx[] = {0};
    constexpr std::string_view kSp[] = {"HCHO", "NO2"};
    constexpr std::string_view kCont[] = {"HCHO NO2"};
    constexpr std::string_view kRev[] = {"01"};
    ASSERT_TRUE(prod1d.AddField(meta, "MainSpecies",
                                prod1d.MakeStringArray(kSpDim, kSp)));
    ASSERT_TRUE(prod1d.AddAttribute(meta, "ProductContents",
                                    prod1d.MakeStringArray(kOne, kCont)));
    ASSERT_TRUE(prod1d.AddAttribute(meta, "Revision",
                                    prod1d.MakeStringArray(kOne, kRev)));
    ASSERT_TRUE(
        prod1d.AddField(geo, "IndexInScan", prod1d.MakeInt32Array(kOne, kIdx)));
    constexpr float k1D[] = {100.0f};
    ASSERT_TRUE(
        AddFloatField(prod1d, hcho, "AveragingKernelPressureLevel", kOne, k1D));
    constexpr long kBad2D[] = {2, 2};
    constexpr float k2D[] = {1.0f, 2.0f, 3.0f, 4.0f};
    ASSERT_TRUE(AddFloatField(prod1d, no2, "AveragingKernelPressureLevel",
                              kBad2D, k2D));

    EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod1d.product(),
                                          "detailed_results=HCHO")
                      .status);
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod1d.product(),
                                          "detailed_results=NO2")
                      .status);
  }

  // 5. read_dataset_for_subindex errors (1D, wrong dim[0], out-of-range
  // subindex) + compound time errors + missing AMF/ESC/QualityFlags datasets.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GOME2", "O3MOTO", 3));
    CodaMemNode* meta = AddChildRecord(prod, prod.root(), "META_DATA");
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    CodaMemNode* det = AddChildRecord(prod, prod.root(), "DETAILED_RESULTS");
    CodaMemNode* so2 = AddChildRecord(prod, det, "SO2");
    constexpr long kOne[] = {1};
    constexpr long kTwo[] = {2};
    constexpr int32_t kIdx[] = {0};
    constexpr std::string_view kSp[] = {"SO2"};
    constexpr std::string_view kCont[] = {"SO2"};
    constexpr std::string_view kRev[] = {"01"};
    ASSERT_TRUE(
        prod.AddField(meta, "MainSpecies", prod.MakeStringArray(kOne, kSp)));
    ASSERT_TRUE(prod.AddAttribute(meta, "ProductContents",
                                  prod.MakeStringArray(kOne, kCont)));
    ASSERT_TRUE(
        prod.AddAttribute(meta, "Revision", prod.MakeStringArray(kOne, kRev)));
    ASSERT_TRUE(
        prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx)));
    constexpr int32_t kDays[] = {18263};
    constexpr int32_t kMillis[] = {0};
    ASSERT_TRUE(prod.AddField(geo, "Time",
                              MakeCompoundTimeArray(prod, kDays, kMillis)));

    // 1D VCDCorrected (expects 2D), wrong dim[0] on AMFTotal, subindex>=dim[1]
    // on ESCCorrected.
    constexpr float k1D[] = {1.0f};
    constexpr long kWrongDim0[] = {2, 2};
    constexpr float k4[] = {1.0f, 2.0f, 3.0f, 4.0f};
    constexpr long kSmallDim1[] = {1, 1};
    ASSERT_TRUE(AddFloatField(prod, so2, "VCDCorrected", kOne, k1D));
    ASSERT_TRUE(AddFloatField(prod, so2, "AMFTotal", kWrongDim0, k4));
    ASSERT_TRUE(AddFloatField(prod, so2, "ESCCorrected", kSmallDim1, k1D));
    // Missing FillValue on AMFTotal_Error.
    ASSERT_TRUE(prod.AddField(so2, "AMFTotal_Error",
                              prod.MakeFloatArray(kWrongDim0, k4)));
    // Missing AMF/ESC/QualityFlags in DETAILED_RESULTS triggers init_* errors.
    IngestExerciseResult res = ExerciseIngestionModule(
        mod_tg, prod.product(), "detailed_results=SO2;corrected=true",
        /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["SO2_column_number_density"].read_status);
    EXPECT_EQ(-1, res.variables["SO2_column_number_density_amf"].read_status);
    EXPECT_EQ(-1, res.variables["SO2_slant_column_number_density"].read_status);
    EXPECT_EQ(-1,
              res.variables["SO2_column_number_density_validity"].read_status);
    // Also check bad subindex=1 on ESCCorrected (dim[1]=1).
    EXPECT_EQ(-1, res.variables["SO2_slant_column_number_density"].read_status);
    // And read_relative_uncertainty failure on AMFTotal_Error (missing
    // AMFTotal) when detailed_results=NO2.
    (void)kTwo;
  }
}

TEST(HarpIngestGome2L2Test, AdditionalErrorBranches) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  const harp_ingestion_module* mod_arp = FindIngestionModule("GOME2_L2_O3MARP");
  const harp_ingestion_module* mod_prof =
      FindIngestionModule("GOME2_L2_O3MNHP");
  const harp_ingestion_module* mod_tg = FindIngestionModule("GOME2_L2_O3MOTO");
  const harp_ingestion_module* mod_ers = FindIngestionModule("GOME_L2_ERSOTO");
  ASSERT_NE(nullptr, mod_arp);
  ASSERT_NE(nullptr, mod_prof);
  ASSERT_NE(nullptr, mod_tg);
  ASSERT_NE(nullptr, mod_ers);

  constexpr long kOne[] = {1};
  constexpr long kTwo[] = {2};
  constexpr int32_t kIdx1[] = {0};
  constexpr int32_t kIdx2[] = {0, 1};
  constexpr std::string_view kRev[] = {"01"};

  // 1. init_window_info & init_revision CODA type/missing errors.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GOME2", "O3MOTO", 3));
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    CodaMemNode* meta = AddChildRecord(prod, prod.root(), "META_DATA");
    ASSERT_TRUE(
        prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx1)));
    // Non-string MainSpecies -> coda_cursor_read_string fails (206-207).
    ASSERT_TRUE(
        prod.AddField(meta, "MainSpecies", prod.MakeInt32Array(kOne, kIdx1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GOME2", "O3MOTO", 3));
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    CodaMemNode* meta = AddChildRecord(prod, prod.root(), "META_DATA");
    ASSERT_TRUE(
        prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx1)));
    constexpr std::string_view kSp[] = {"O3"};
    ASSERT_TRUE(
        prod.AddField(meta, "MainSpecies", prod.MakeStringArray(kOne, kSp)));
    // Missing @ProductContents[0] (230-231).
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GOME2", "O3MOTO", 3));
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    CodaMemNode* meta = AddChildRecord(prod, prod.root(), "META_DATA");
    ASSERT_TRUE(
        prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx1)));
    constexpr std::string_view kSp[] = {"O3"};
    ASSERT_TRUE(
        prod.AddField(meta, "MainSpecies", prod.MakeStringArray(kOne, kSp)));
    // Non-string @ProductContents[0] (235-236).
    ASSERT_TRUE(prod.AddAttribute(meta, "ProductContents",
                                  prod.MakeInt32Array(kOne, kIdx1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GOME2", "O3MOTO", 3));
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    CodaMemNode* meta = AddChildRecord(prod, prod.root(), "META_DATA");
    ASSERT_TRUE(
        prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx1)));
    constexpr std::string_view kSp[] = {"O3"};
    ASSERT_TRUE(
        prod.AddField(meta, "MainSpecies", prod.MakeStringArray(kOne, kSp)));
    ASSERT_TRUE(prod.AddAttribute(meta, "ProductContents",
                                  prod.MakeStringArray(kOne, kSp)));
    // Missing @Revision[0] (264-265).
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GOME2", "O3MOTO", 3));
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    CodaMemNode* meta = AddChildRecord(prod, prod.root(), "META_DATA");
    ASSERT_TRUE(
        prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx1)));
    constexpr std::string_view kSp[] = {"O3"};
    ASSERT_TRUE(
        prod.AddField(meta, "MainSpecies", prod.MakeStringArray(kOne, kSp)));
    ASSERT_TRUE(prod.AddAttribute(meta, "ProductContents",
                                  prod.MakeStringArray(kOne, kSp)));
    // Non-string @Revision[0] (269-270).
    ASSERT_TRUE(
        prod.AddAttribute(meta, "Revision", prod.MakeInt32Array(kOne, kIdx1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod.product()).status);
  }

  // 2. Aerosol string-time errors (885-886, 909-910, 1137-1174) and FillValue
  // wrong type (382-383). Guarded under MSan because `ingestion_init_aerosol`
  // leaves `info->num_vertical` uninitialized before `read_dimensions`.
#if !defined(MEMORY_SANITIZER)
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    ASSERT_TRUE(
        prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx1)));
    ASSERT_TRUE(prod.AddField(geo, "Time", prod.MakeInt32Array(kOne, kIdx1)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_arp, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    ASSERT_TRUE(
        prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kTwo, kIdx2)));
    // First time valid so datetime_start passes, second invalid so
    // datetime_stop fails (909-910).
    constexpr std::string_view kBadLast[] = {"2020-05-15T12:00:00.000",
                                             "invalid-last-time"};
    ASSERT_TRUE(
        prod.AddField(geo, "Time", prod.MakeStringArray(kTwo, kBadLast)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod_arp, prod.product()).status);
  }
  {
    // Non-string FillValue on float field + read_time_string errors.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    ASSERT_TRUE(
        prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kTwo, kIdx2)));
    constexpr std::string_view kGood2[] = {"2020-05-15T12:00:00.000",
                                           "2020-05-15T12:00:01.000"};
    ASSERT_TRUE(prod.AddField(geo, "Time", prod.MakeStringArray(kTwo, kGood2)));
    constexpr float kF2[] = {1.0f, 2.0f};
    CodaMemNode* lon = prod.MakeFloatArray(kTwo, kF2);
    constexpr std::string_view kBadFill[] = {"nan"};
    ASSERT_TRUE(prod.AddAttribute(lon, "FillValue",
                                  prod.MakeStringArray(kOne, kBadFill)));
    ASSERT_TRUE(prod.AddField(geo, "LongitudeCenter", lon));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod_arp, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
  }
#endif  // !defined(MEMORY_SANITIZER)

  // 3. read_datetime_range_compound & read_time_compound error paths.
  {
    auto make_tg_base = [&](int version, CodaMemProduct& prod,
                            CodaMemNode*& geo, CodaMemNode*& meta,
                            CodaMemNode*& tot, CodaMemNode*& det) {
      ASSERT_TRUE(prod.ok());
      ASSERT_TRUE(prod.SetProductInfo("GOME2", "O3MOTO", version));
      meta = AddChildRecord(prod, prod.root(), "META_DATA");
      geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
      tot = AddChildRecord(prod, prod.root(), "TOTAL_COLUMNS");
      det = AddChildRecord(prod, prod.root(), "DETAILED_RESULTS");
      constexpr std::string_view kSp[] = {"BrO", "H2O",  "HCHO", "NO2",
                                          "O3",  "OClO", "SO2"};
      constexpr long kSpDim[] = {7};
      ASSERT_TRUE(prod.AddField(meta, "MainSpecies",
                                prod.MakeStringArray(kSpDim, kSp)));
      constexpr std::string_view kCont[] = {"BrO H2O HCHO NO2 O3 OClO SO2"};
      ASSERT_TRUE(prod.AddAttribute(meta, "ProductContents",
                                    prod.MakeStringArray(kOne, kCont)));
      ASSERT_TRUE(prod.AddAttribute(meta, "Revision",
                                    prod.MakeStringArray(kOne, kRev)));
    };

    // 3a. Missing GEOLOCATION/Time -> read_datetime_range_compound fails.
    {
      CodaMemProduct prod;
      CodaMemNode *geo, *meta, *tot, *det;
      make_tg_base(3, prod, geo, meta, tot, det);
      ASSERT_TRUE(
          prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx1)));
      EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod.product()).status);
    }

    // 3b. Scalar Time element (not record) -> first_record_field fails.
    {
      CodaMemProduct prod;
      CodaMemNode *geo, *meta, *tot, *det;
      make_tg_base(3, prod, geo, meta, tot, det);
      ASSERT_TRUE(
          prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx1)));
      ASSERT_TRUE(prod.AddField(geo, "Time", prod.MakeInt32Array(kOne, kIdx1)));
      EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod.product()).status);
    }

    // 3c. First record field wrong type (string Day).
    {
      CodaMemProduct prod;
      CodaMemNode *geo, *meta, *tot, *det;
      make_tg_base(3, prod, geo, meta, tot, det);
      ASSERT_TRUE(
          prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx1)));
      CodaMemNode* rec = prod.MakeRecord();
      ASSERT_TRUE(prod.AddField(rec, "Day", prod.MakeString("bad")));
      CodaMemNode* elems[] = {rec};
      ASSERT_TRUE(prod.AddField(geo, "Time", prod.MakeNodeArray(kOne, elems)));
      EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod.product()).status);
    }

    // 3d. Record with only 1 field (missing MillisecondOfDay).
    {
      CodaMemProduct prod;
      CodaMemNode *geo, *meta, *tot, *det;
      make_tg_base(3, prod, geo, meta, tot, det);
      ASSERT_TRUE(
          prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx1)));
      CodaMemNode* rec = prod.MakeRecord();
      ASSERT_TRUE(prod.AddField(rec, "Day", prod.MakeInt32(18263)));
      CodaMemNode* elems[] = {rec};
      ASSERT_TRUE(prod.AddField(geo, "Time", prod.MakeNodeArray(kOne, elems)));
      EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod.product()).status);
    }

    // 3e. Second record field wrong type (string MillisecondOfDay).
    {
      CodaMemProduct prod;
      CodaMemNode *geo, *meta, *tot, *det;
      make_tg_base(3, prod, geo, meta, tot, det);
      ASSERT_TRUE(
          prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx1)));
      CodaMemNode* rec = prod.MakeRecord();
      ASSERT_TRUE(prod.AddField(rec, "Day", prod.MakeInt32(18263)));
      ASSERT_TRUE(
          prod.AddField(rec, "MillisecondOfDay", prod.MakeString("bad")));
      CodaMemNode* elems[] = {rec};
      ASSERT_TRUE(prod.AddField(geo, "Time", prod.MakeNodeArray(kOne, elems)));
      EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod.product()).status);
    }

    // 3f. First Time record valid, second Time record bad (hits datetime_stop
    // error lines 822-838).
    {
      CodaMemProduct prod;
      CodaMemNode *geo, *meta, *tot, *det;
      make_tg_base(3, prod, geo, meta, tot, det);
      ASSERT_TRUE(
          prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kTwo, kIdx2)));
      CodaMemNode* r0 = prod.MakeRecord();
      ASSERT_TRUE(prod.AddField(r0, "Day", prod.MakeInt32(18263)));
      ASSERT_TRUE(prod.AddField(r0, "MillisecondOfDay", prod.MakeInt32(0)));
      CodaMemNode* r1 = prod.MakeRecord();
      ASSERT_TRUE(prod.AddField(r1, "Day", prod.MakeString("bad")));
      CodaMemNode* elems[] = {r0, r1};
      ASSERT_TRUE(prod.AddField(geo, "Time", prod.MakeNodeArray(kTwo, elems)));
      EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod.product()).status);
    }
    {
      CodaMemProduct prod;
      CodaMemNode *geo, *meta, *tot, *det;
      make_tg_base(3, prod, geo, meta, tot, det);
      ASSERT_TRUE(
          prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kTwo, kIdx2)));
      CodaMemNode* r0 = prod.MakeRecord();
      ASSERT_TRUE(prod.AddField(r0, "Day", prod.MakeInt32(18263)));
      ASSERT_TRUE(prod.AddField(r0, "MillisecondOfDay", prod.MakeInt32(0)));
      CodaMemNode* r1 = prod.MakeRecord();
      ASSERT_TRUE(prod.AddField(r1, "Day", prod.MakeInt32(18264)));
      CodaMemNode* elems[] = {r0, r1};
      ASSERT_TRUE(prod.AddField(geo, "Time", prod.MakeNodeArray(kTwo, elems)));
      EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod.product()).status);
    }
    {
      CodaMemProduct prod;
      CodaMemNode *geo, *meta, *tot, *det;
      make_tg_base(3, prod, geo, meta, tot, det);
      ASSERT_TRUE(
          prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kTwo, kIdx2)));
      CodaMemNode* r0 = prod.MakeRecord();
      ASSERT_TRUE(prod.AddField(r0, "Day", prod.MakeInt32(18263)));
      ASSERT_TRUE(prod.AddField(r0, "MillisecondOfDay", prod.MakeInt32(0)));
      CodaMemNode* r1 = prod.MakeRecord();
      ASSERT_TRUE(prod.AddField(r1, "Day", prod.MakeInt32(18264)));
      ASSERT_TRUE(
          prod.AddField(r1, "MillisecondOfDay", prod.MakeString("bad")));
      CodaMemNode* elems[] = {r0, r1};
      ASSERT_TRUE(prod.AddField(geo, "Time", prod.MakeNodeArray(kTwo, elems)));
      EXPECT_EQ(-1, ExerciseIngestionModule(mod_tg, prod.product()).status);
    }

    // 4. All variable read error paths when datasets are missing/wrong on
    // trace gases (v3, detailed_results=BrO, plus ERSOTO for IndexInScan type
    // failure, plus v2 for O3/SO2/NO2 error branches).
    {
      CodaMemProduct prod;
      CodaMemNode *geo, *meta, *tot, *det;
      make_tg_base(3, prod, geo, meta, tot, det);
      // String IndexInScan of length 1: init_num_main succeeds,
      // init_index_in_scan fails on read_dataset(int32)!
      constexpr std::string_view kStrIdx[] = {"bad"};
      ASSERT_TRUE(prod.AddField(geo, "IndexInScan",
                                prod.MakeStringArray(kOne, kStrIdx)));
      constexpr int32_t kDays[] = {18263};
      constexpr int32_t kMillis[] = {0};
      ASSERT_TRUE(prod.AddField(geo, "Time",
                                MakeCompoundTimeArray(prod, kDays, kMillis)));
      constexpr float kF1[] = {1.0f};
      // Populate LongitudeB (missing D/C/A), LatitudeB (missing D/C/A),
      // TOTAL_COLUMNS/H2O (missing H2O_Error for read_relative_uncertainty),
      // and DETAILED_RESULTS/AMFTotal + ESC (missing AMFTotal_Error / ESC_Error
      // for init_amf_error / init_esc_error).
      ASSERT_TRUE(AddFloatField(prod, geo, "LongitudeB", kOne, kF1));
      ASSERT_TRUE(AddFloatField(prod, geo, "LatitudeB", kOne, kF1));
      ASSERT_TRUE(AddFloatField(prod, tot, "H2O", kOne, kF1));
      constexpr long kWin1[] = {1, 7};
      constexpr float kWin7[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
      ASSERT_TRUE(AddFloatField(prod, det, "AMFTotal", kWin1, kWin7));
      ASSERT_TRUE(AddFloatField(prod, det, "ESC", kWin1, kWin7));

      // On O3MOTO, scan_subindex reads SubpixelInScan (not IndexInScan), so
      // scan_direction_type is the sole caller of init_index_in_scan.
      IngestExerciseResult res_tg = ExerciseIngestionModule(
          mod_tg, prod.product(), "detailed_results=BrO",
          /*stop_on_error=*/false);
      EXPECT_EQ(-1, res_tg.status);
      EXPECT_EQ(-1, res_tg.variables["scan_subindex"].read_status);
      EXPECT_EQ(-1, res_tg.variables["scan_direction_type"].read_status);
      EXPECT_EQ(-1, res_tg.variables["longitude_bounds"].read_status);
      EXPECT_EQ(-1, res_tg.variables["latitude_bounds"].read_status);
      EXPECT_EQ(-1,
                res_tg.variables["H2O_column_density_uncertainty"].read_status);
      EXPECT_EQ(-1,
                res_tg.variables["BrO_column_number_density_amf_uncertainty"]
                    .read_status);
      EXPECT_EQ(-1,
                res_tg.variables["BrO_slant_column_number_density_uncertainty"]
                    .read_status);
      EXPECT_EQ(-1, res_tg.variables["O3_column_number_density"].read_status);
      EXPECT_EQ(
          -1,
          res_tg.variables["O3_column_number_density_uncertainty"].read_status);
      EXPECT_EQ(-1, res_tg.variables["SO2_column_number_density"].read_status);
      EXPECT_EQ(-1, res_tg.variables["SO2_column_number_density_uncertainty"]
                        .read_status);
    }
    // 4b. ERSOTO scan_subindex failure when IndexInScan is non-int32 (use
    // stop_on_error=true with all prior common variables present so
    // scan_direction_type is not called after failed init_index_in_scan).
    {
      CodaMemProduct prod;
      ASSERT_TRUE(prod.ok());
      ASSERT_TRUE(prod.SetProductInfo("GOME2", "O3MOTO", 3));
      CodaMemNode* meta = AddChildRecord(prod, prod.root(), "META_DATA");
      CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
      CodaMemNode* det = AddChildRecord(prod, prod.root(), "DETAILED_RESULTS");
      CodaMemNode* cloud =
          AddChildRecord(prod, prod.root(), "CLOUD_PROPERTIES");
      constexpr long kZero[] = {0};
      ASSERT_TRUE(prod.AddField(
          meta, "MainSpecies",
          prod.MakeStringArray(kZero, std::span<const std::string_view>{})));
      constexpr std::string_view kCont[] = {""};
      ASSERT_TRUE(prod.AddAttribute(meta, "ProductContents",
                                    prod.MakeStringArray(kOne, kCont)));
      ASSERT_TRUE(prod.AddAttribute(meta, "Revision",
                                    prod.MakeStringArray(kOne, kRev)));
      constexpr std::string_view kStrIdx[] = {"bad"};
      ASSERT_TRUE(prod.AddField(geo, "IndexInScan",
                                prod.MakeStringArray(kOne, kStrIdx)));
      constexpr int32_t kDays[] = {18263};
      constexpr int32_t kMillis[] = {0};
      ASSERT_TRUE(prod.AddField(geo, "Time",
                                MakeCompoundTimeArray(prod, kDays, kMillis)));
      constexpr float kF1[] = {1.0f};
      for (std::string_view f :
           {"LongitudeCentre", "LatitudeCentre", "LongitudeA", "LongitudeB",
            "LongitudeC", "LongitudeD", "LatitudeA", "LatitudeB", "LatitudeC",
            "LatitudeD", "SolarZenithAngleSatCentre", "SolarZenithAngleCentre",
            "LineOfSightZenithAngleCentre", "RelativeAzimuthCentre"}) {
        ASSERT_TRUE(AddFloatField(prod, geo, f, kOne, kF1));
      }
      for (std::string_view f :
           {"CloudFraction", "CloudFraction_Error", "CloudTopPressure",
            "CloudTopPressure_Error", "CloudTopHeight", "CloudTopHeight_Error",
            "CloudTopAlbedo", "CloudTopAlbedo_Error", "CloudOpticalThickness",
            "CloudOpticalThickness_Error"}) {
        ASSERT_TRUE(AddFloatField(prod, cloud, f, kOne, kF1));
      }
      for (std::string_view f : {"AAI", "SurfaceHeight", "SurfacePressure"}) {
        ASSERT_TRUE(AddFloatField(prod, det, f, kOne, kF1));
      }
      IngestExerciseResult res_ers = ExerciseIngestionModule(
          mod_ers, prod.product(), nullptr, /*stop_on_error=*/true);
      EXPECT_EQ(-1, res_ers.status);
      EXPECT_EQ("scan_subindex", res_ers.failed_variable);
    }

    // 5. Missing AMFTotal, ESC, QualityFlags, and v2 O3/SO2 error paths +
    // missing H2O_Flag / HCHO_Flag / O3_Volcano_Flag / OClO_Flag / SO2_Flag /
    // SO2_Volcano_Flag when QualityFlags is present.
    {
      CodaMemProduct prod;
      CodaMemNode *geo, *meta, *tot, *det;
      make_tg_base(3, prod, geo, meta, tot, det);
      ASSERT_TRUE(
          prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx1)));
      constexpr int32_t kDays[] = {18263};
      constexpr int32_t kMillis[] = {0};
      ASSERT_TRUE(prod.AddField(geo, "Time",
                                MakeCompoundTimeArray(prod, kDays, kMillis)));
      constexpr long kWin1[] = {1, 7};
      constexpr int32_t kQf7[] = {0, 1, 2, 3, 4, 5, 6};
      ASSERT_TRUE(
          prod.AddField(det, "QualityFlags", prod.MakeInt32Array(kWin1, kQf7)));
      // Add SO2_Flag but omit SO2_Volcano_Flag to cover line 2247-2248.
      CodaMemNode* so2 = AddChildRecord(prod, det, "SO2");
      ASSERT_TRUE(
          prod.AddField(so2, "SO2_Flag", prod.MakeInt32Array(kOne, kIdx1)));
      // Missing AMFTotal, ESC, H2O_Flag, HCHO_Flag, O3_Volcano_Flag, OClO_Flag.
      IngestExerciseResult res = ExerciseIngestionModule(
          mod_tg, prod.product(), "detailed_results=BrO",
          /*stop_on_error=*/false);
      EXPECT_EQ(-1, res.status);
      EXPECT_EQ(-1, res.variables["BrO_column_number_density_amf"].read_status);
      EXPECT_EQ(-1, res.variables["BrO_column_number_density_amf_uncertainty"]
                        .read_status);
      EXPECT_EQ(-1,
                res.variables["BrO_slant_column_number_density"].read_status);
      EXPECT_EQ(-1, res.variables["BrO_slant_column_number_density_uncertainty"]
                        .read_status);
      EXPECT_EQ(
          -1, res.variables["H2O_column_number_density_validity"].read_status);
      EXPECT_EQ(
          -1, res.variables["HCHO_column_number_density_validity"].read_status);
      EXPECT_EQ(-1,
                res.variables["O3_column_number_density_validity"].read_status);
      EXPECT_EQ(
          -1, res.variables["OClO_column_number_density_validity"].read_status);
      EXPECT_EQ(
          -1, res.variables["SO2_column_number_density_validity"].read_status);
    }

    // 6. HCHO and NO2 v3 profile variables (pressure, apriori, avk) missing
    // Apriori / AveragingKernel / wrong read_dataset_for_subindex dims/type.
    {
      CodaMemProduct prod;
      CodaMemNode *geo, *meta, *tot, *det;
      make_tg_base(3, prod, geo, meta, tot, det);
      ASSERT_TRUE(
          prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx1)));
      constexpr int32_t kDays[] = {18263};
      constexpr int32_t kMillis[] = {0};
      ASSERT_TRUE(prod.AddField(geo, "Time",
                                MakeCompoundTimeArray(prod, kDays, kMillis)));
      CodaMemNode* hcho = AddChildRecord(prod, det, "HCHO");
      CodaMemNode* no2 = AddChildRecord(prod, det, "NO2");
      CodaMemNode* so2 = AddChildRecord(prod, det, "SO2");
      constexpr long kV2[] = {1, 2};
      constexpr float kP2[] = {1000.0f, 500.0f};
      ASSERT_TRUE(
          AddFloatField(prod, hcho, "AveragingKernelPressureLevel", kV2, kP2));
      ASSERT_TRUE(
          AddFloatField(prod, no2, "AveragingKernelPressureLevel", kV2, kP2));
      // 2D string array on SO2/VCDCorrected to trigger
      // coda_cursor_read_float_array error in read_dataset_for_subindex.
      constexpr long kSo2Dim[] = {1, 2};
      constexpr std::string_view kBadSo2[] = {"a", "b"};
      ASSERT_TRUE(prod.AddField(so2, "VCDCorrected",
                                prod.MakeStringArray(kSo2Dim, kBadSo2)));
      // Missing @FillValue[0] on SO2/AMFTotal.
      ASSERT_TRUE(
          prod.AddField(so2, "AMFTotal", prod.MakeFloatArray(kSo2Dim, kP2)));

      IngestExerciseResult res_hcho = ExerciseIngestionModule(
          mod_tg, prod.product(), "detailed_results=HCHO",
          /*stop_on_error=*/false);
      EXPECT_EQ(-1,
                res_hcho.variables["HCHO_volume_mixing_ratio_dry_air_apriori"]
                    .read_status);
      EXPECT_EQ(
          -1, res_hcho.variables["HCHO_column_number_density_avk"].read_status);

      IngestExerciseResult res_no2 = ExerciseIngestionModule(
          mod_tg, prod.product(), "detailed_results=NO2",
          /*stop_on_error=*/false);
      EXPECT_EQ(-1, res_no2.variables["NO2_volume_mixing_ratio_dry_air_apriori"]
                        .read_status);
      EXPECT_EQ(-1,
                res_no2.variables["NO2_column_number_density_avk"].read_status);

      IngestExerciseResult res_so2 = ExerciseIngestionModule(
          mod_tg, prod.product(), "detailed_results=SO2;corrected=true",
          /*stop_on_error=*/false);
      EXPECT_EQ(-1, res_so2.variables["SO2_column_number_density"].read_status);
      EXPECT_EQ(-1,
                res_so2.variables["SO2_column_number_density_amf"].read_status);
    }

    // 7. Version 2 missing O3/SO2/O3_Error/SO2_Error.
    {
      CodaMemProduct prod;
      CodaMemNode *geo, *meta, *tot, *det;
      make_tg_base(2, prod, geo, meta, tot, det);
      ASSERT_TRUE(
          prod.AddField(geo, "IndexInScan", prod.MakeInt32Array(kOne, kIdx1)));
      constexpr int32_t kDays[] = {18263};
      constexpr int32_t kMillis[] = {0};
      ASSERT_TRUE(prod.AddField(geo, "Time",
                                MakeCompoundTimeArray(prod, kDays, kMillis)));
      IngestExerciseResult res = ExerciseIngestionModule(
          mod_tg, prod.product(), nullptr, /*stop_on_error=*/false);
      EXPECT_EQ(
          -1,
          res.variables["O3_column_number_density_uncertainty"].read_status);
      EXPECT_EQ(
          -1,
          res.variables["SO2_column_number_density_uncertainty"].read_status);
    }
  }

  // 4. Profile variable read errors (missing Longitude_B/D/C/A,
  // Latitude_B/D/C/A, OutputPressureGrid, StateRetrieved, Apriori,
  // AveragingKernel, QualityProcessing).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* geo = AddChildRecord(prod, prod.root(), "GEOLOCATION");
    CodaMemNode* data = AddChildRecord(prod, prod.root(), "DATA");
    constexpr std::string_view kGood2[] = {"2020-05-15T12:00:00.000",
                                           "2020-05-15T12:00:01.000"};
    ASSERT_TRUE(prod.AddField(geo, "Time", prod.MakeStringArray(kTwo, kGood2)));
    constexpr long kDimsState[] = {3, 2};
    constexpr float kStateDef[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    ASSERT_TRUE(AddFloatField(prod, data, "StateDef", kDimsState, kStateDef));
    ASSERT_TRUE(
        AddFloatField(prod, data, "AltitudeProfile", kDimsState, kStateDef));
    constexpr float kF2[] = {1.0f, 2.0f};
    ASSERT_TRUE(AddFloatField(prod, geo, "Longitude_B", kTwo, kF2));
    ASSERT_TRUE(AddFloatField(prod, geo, "Latitude_B", kTwo, kF2));

    IngestExerciseResult res = ExerciseModuleSkippingVar(
        mod_prof, prod.product(), "scan_subindex", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    for (const auto& [name, var] : res.variables) {
      if (name != "datetime" && name != "altitude_bounds" && name != "index") {
        EXPECT_EQ(-1, var.read_status) << name;
      }
    }
  }
}

}  // namespace
}  // namespace stcorp_harp
