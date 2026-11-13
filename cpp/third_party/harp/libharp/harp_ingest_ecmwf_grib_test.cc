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
// Unit tests for ECMWF GRIB ingestion module
// (libharp/harp-ingest-ecmwf-grib.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-ecmwf-grib.c

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <optional>
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

using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::FloatNear;
using ::testing::HasSubstr;

constexpr std::string_view kGribUnionFields[] = {"grib1", "grib2"};

struct GridSpec {
  bool is_gaussian = false;
  uint32_t ni = 2;
  uint32_t nj = 2;
  int32_t lat_first = 50000;
  int32_t lon_first = 10000;
  int32_t lat_last = 40000;
  int32_t lon_last = 20000;
  uint32_t di = 10000;
  uint32_t dj = 10000;
  uint32_t n = 1;
  uint8_t scanning_mode = 0;
  std::vector<double> coordinate_values;
};

struct Grib1MsgSpec {
  uint8_t century = 21;
  uint8_t year_of_century = 20;
  uint8_t month = 5;
  uint8_t day = 15;
  uint8_t hour = 12;
  uint8_t minute = 30;
  uint8_t unit_of_time_range = 0;
  uint8_t p1 = 0;
  uint8_t table2_version = 128;
  uint8_t indicator_of_parameter = 129;
  uint8_t indicator_of_type_of_level = 1;
  uint16_t level = 1;
  GridSpec grid;
  std::vector<float> values = {1.0f, 2.0f, 3.0f, 4.0f};
};

struct Grib2DataSpec {
  uint8_t parameter_category = 0;
  uint8_t parameter_number = 0;
  std::optional<uint16_t> constituent_type;
  uint8_t unit_of_time_range = 1;
  uint32_t forecast_time = 0;
  uint8_t type_of_first_fixed_surface = 105;
  double first_fixed_surface = 1.0;
  std::vector<double> coordinate_values;
  std::vector<float> values = {1.0f, 2.0f, 3.0f, 4.0f};
};

struct Grib2MsgSpec {
  uint8_t significance_of_reference_time = 1;
  uint16_t year = 2020;
  uint8_t month = 5;
  uint8_t day = 15;
  uint8_t hour = 12;
  uint8_t minute = 30;
  uint8_t second = 0;
  uint8_t discipline = 0;
  GridSpec grid = {
      .is_gaussian = false,
      .ni = 2,
      .nj = 2,
      .lat_first = 50000000,
      .lon_first = 10000000,
      .lat_last = 40000000,
      .lon_last = 20000000,
      .di = 10000000,
      .dj = 10000000,
      .n = 1,
      .scanning_mode = 0,
  };
  std::vector<Grib2DataSpec> data;
};

enum class MutScope { kNone, kGrid, kMsg, kData };
enum class MutMode { kTruncateBefore, kOmitOne, kBadType, kBadScalar };
struct FieldMut {
  MutScope scope = MutScope::kNone;
  MutMode mode = MutMode::kTruncateBefore;
  std::string_view field;
};

template <typename MakeFn>
bool AddMaybeMutated(CodaMemProduct& prod, CodaMemNode* rec,
                     std::string_view name, MutScope current_scope,
                     const FieldMut& mut, bool* truncated, MakeFn make_fn) {
  if (mut.scope == current_scope && mut.field == name) {
    if (mut.mode == MutMode::kTruncateBefore) {
      *truncated = true;
      return true;
    }
    if (mut.mode == MutMode::kOmitOne) {
      return true;
    }
    if (mut.mode == MutMode::kBadScalar) {
      return prod.AddField(rec, name, prod.MakeString("bad"));
    }
    if (mut.mode == MutMode::kBadType) {
      if (name == "coordinateValues") {
        const long dims[] = {4};
        constexpr std::string_view kBadStrs[] = {"a", "b", "c", "d"};
        return prod.AddField(rec, name, prod.MakeStringArray(dims, kBadStrs));
      }
      return prod.AddField(rec, name, prod.MakeString("bad"));
    }
  }
  CodaMemNode* node = make_fn();
  return node != nullptr && prod.AddField(rec, name, node);
}

CodaMemNode* BuildGridRecord(CodaMemProduct& prod, int grib_version,
                             const GridSpec& g, const FieldMut& mut = {}) {
  CodaMemNode* grid = prod.MakeRecord();
  if (grid == nullptr) return nullptr;
  bool trunc = false;
  auto add = [&](std::string_view name, auto fn) -> bool {
    return AddMaybeMutated(prod, grid, name, MutScope::kGrid, mut, &trunc, fn);
  };

  if (grib_version == 1) {
    if (!add("dataRepresentationType",
             [&] { return prod.MakeUint8(g.is_gaussian ? 4 : 0); })) {
      return nullptr;
    }
  } else {
    if (!add("gridDefinitionTemplateNumber",
             [&] { return prod.MakeUint16(g.is_gaussian ? 40 : 0); })) {
      return nullptr;
    }
  }
  if (trunc) return grid;
  if (!add("Ni", [&] { return prod.MakeUint32(g.ni); }) || trunc) {
    return trunc ? grid : nullptr;
  }
  if (!add("Nj", [&] { return prod.MakeUint32(g.nj); }) || trunc) {
    return trunc ? grid : nullptr;
  }
  if (!add("latitudeOfFirstGridPoint",
           [&] { return prod.MakeInt32(g.lat_first); }) ||
      trunc) {
    return trunc ? grid : nullptr;
  }
  if (!add("longitudeOfFirstGridPoint",
           [&] { return prod.MakeInt32(g.lon_first); }) ||
      trunc) {
    return trunc ? grid : nullptr;
  }
  if (!add("latitudeOfLastGridPoint",
           [&] { return prod.MakeInt32(g.lat_last); }) ||
      trunc) {
    return trunc ? grid : nullptr;
  }
  if (!add("longitudeOfLastGridPoint",
           [&] { return prod.MakeInt32(g.lon_last); }) ||
      trunc) {
    return trunc ? grid : nullptr;
  }
  if (!add("iDirectionIncrement", [&] { return prod.MakeUint32(g.di); }) ||
      trunc) {
    return trunc ? grid : nullptr;
  }
  if (g.is_gaussian) {
    if (!add("N", [&] { return prod.MakeUint32(g.n); }) || trunc) {
      return trunc ? grid : nullptr;
    }
  } else {
    if (!add("jDirectionIncrement", [&] { return prod.MakeUint32(g.dj); }) ||
        trunc) {
      return trunc ? grid : nullptr;
    }
  }
  if (!add("scanningMode", [&] { return prod.MakeUint8(g.scanning_mode); }) ||
      trunc) {
    return trunc ? grid : nullptr;
  }
  if (!g.coordinate_values.empty()) {
    const long dims[] = {static_cast<long>(g.coordinate_values.size())};
    if (!add("coordinateValues",
             [&] { return prod.MakeDoubleArray(dims, g.coordinate_values); })) {
      return nullptr;
    }
  }
  return grid;
}

CodaMemNode* BuildGrib1UnionMsg(CodaMemProduct& prod, const Grib1MsgSpec& m,
                                const FieldMut& mut = {}) {
  CodaMemNode* msg = prod.MakeRecord();
  if (msg == nullptr) return nullptr;
  bool trunc = false;
  auto add = [&](std::string_view name, auto fn) -> bool {
    return AddMaybeMutated(prod, msg, name, MutScope::kMsg, mut, &trunc, fn);
  };
  auto finish = [&] {
    return prod.MakeUnion(kGribUnionFields, /*active_index=*/0, msg);
  };

  // Order puts by-name fields before consecutive goto_next groups
  // (unitOfTimeRange->P1 and yearOfCentury->minute) so truncating before
  // any consecutive field leaves its predecessor as the last field.
  if (!add("centuryOfReferenceTimeOfData",
           [&] { return prod.MakeUint8(m.century); }) ||
      trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("table2Version", [&] { return prod.MakeUint8(m.table2_version); }) ||
      trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("indicatorOfParameter",
           [&] { return prod.MakeUint8(m.indicator_of_parameter); }) ||
      trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("indicatorOfTypeOfLevel",
           [&] { return prod.MakeUint8(m.indicator_of_type_of_level); }) ||
      trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("level", [&] { return prod.MakeUint16(m.level); }) || trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("grid", [&] { return BuildGridRecord(prod, 1, m.grid, mut); }) ||
      trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("data",
           [&] {
             CodaMemNode* data = prod.MakeRecord();
             if (data == nullptr) return static_cast<CodaMemNode*>(nullptr);
             bool dtrunc = false;
             const long val_dims[] = {static_cast<long>(m.values.size())};
             if (!AddMaybeMutated(
                     prod, data, "values", MutScope::kData, mut, &dtrunc,
                     [&] { return prod.MakeFloatArray(val_dims, m.values); })) {
               return static_cast<CodaMemNode*>(nullptr);
             }
             return data;
           }) ||
      trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("unitOfTimeRange",
           [&] { return prod.MakeUint8(m.unit_of_time_range); }) ||
      trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("P1", [&] { return prod.MakeUint8(m.p1); }) || trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("yearOfCentury",
           [&] { return prod.MakeUint8(m.year_of_century); }) ||
      trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("month", [&] { return prod.MakeUint8(m.month); }) || trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("day", [&] { return prod.MakeUint8(m.day); }) || trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("hour", [&] { return prod.MakeUint8(m.hour); }) || trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("minute", [&] { return prod.MakeUint8(m.minute); })) {
    return nullptr;
  }
  return finish();
}

CodaMemNode* BuildGrib2UnionMsg(CodaMemProduct& prod, const Grib2MsgSpec& m,
                                const FieldMut& mut = {},
                                bool omit_constituent_field = false) {
  CodaMemNode* msg = prod.MakeRecord();
  if (msg == nullptr) return nullptr;
  bool trunc = false;
  auto add = [&](std::string_view name, auto fn) -> bool {
    return AddMaybeMutated(prod, msg, name, MutScope::kMsg, mut, &trunc, fn);
  };
  auto finish = [&] {
    return prod.MakeUnion(kGribUnionFields, /*active_index=*/1, msg);
  };

  auto build_data_arr = [&]() -> CodaMemNode* {
    std::vector<CodaMemNode*> data_elems;
    data_elems.reserve(m.data.size());
    for (const auto& d : m.data) {
      CodaMemNode* drec = prod.MakeRecord();
      if (drec == nullptr) return nullptr;
      bool dtrunc = false;
      auto dadd = [&](std::string_view name, auto fn) -> bool {
        return AddMaybeMutated(prod, drec, name, MutScope::kData, mut, &dtrunc,
                               fn);
      };
      if (!dadd("parameterCategory",
                [&] { return prod.MakeUint8(d.parameter_category); }) ||
          dtrunc ||
          !dadd("parameterNumber",
                [&] { return prod.MakeUint8(d.parameter_number); }) ||
          dtrunc) {
        if (!dtrunc) return nullptr;
        data_elems.push_back(drec);
        continue;
      }
      if (!omit_constituent_field) {
        if (mut.scope == MutScope::kData && mut.field == "constituentType" &&
            mut.mode == MutMode::kBadType) {
          if (!prod.AddField(drec, "constituentType", prod.MakeString("bad"))) {
            return nullptr;
          }
        } else if (d.constituent_type.has_value()) {
          if (!prod.AddField(drec, "constituentType",
                             prod.MakeUint16(*d.constituent_type))) {
            return nullptr;
          }
        } else if (!prod.AddUnavailableField(drec, "constituentType")) {
          return nullptr;
        }
      }
      if (!dadd("indicatorOfUnitOfTimeRange",
                [&] { return prod.MakeUint8(d.unit_of_time_range); }) ||
          dtrunc ||
          !dadd("forecastTime",
                [&] { return prod.MakeUint32(d.forecast_time); }) ||
          dtrunc ||
          !dadd(
              "typeOfFirstFixedSurface",
              [&] { return prod.MakeUint8(d.type_of_first_fixed_surface); }) ||
          dtrunc ||
          !dadd("firstFixedSurface",
                [&] { return prod.MakeDouble(d.first_fixed_surface); }) ||
          dtrunc) {
        if (!dtrunc) return nullptr;
        data_elems.push_back(drec);
        continue;
      }
      if (!d.coordinate_values.empty()) {
        const long cdims[] = {static_cast<long>(d.coordinate_values.size())};
        if (!dadd("coordinateValues",
                  [&] {
                    return prod.MakeDoubleArray(cdims, d.coordinate_values);
                  }) ||
            dtrunc) {
          if (!dtrunc) return nullptr;
          data_elems.push_back(drec);
          continue;
        }
      }
      const long vdims[] = {static_cast<long>(d.values.size())};
      if (!dadd("values",
                [&] { return prod.MakeFloatArray(vdims, d.values); }) &&
          !dtrunc) {
        return nullptr;
      }
      data_elems.push_back(drec);
    }
    const long data_dims[] = {static_cast<long>(data_elems.size())};
    return prod.MakeNodeArray(data_dims, data_elems);
  };

  // Place "data" first so get_num_grid_data finds it while truncating
  // significanceOfReferenceTime..second still leaves the prior field last.
  if (!add("data", build_data_arr) || trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("discipline", [&] { return prod.MakeUint8(m.discipline); }) ||
      trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("grid",
           [&] {
             CodaMemNode* grid_rec = BuildGridRecord(prod, 2, m.grid, mut);
             if (grid_rec == nullptr) return static_cast<CodaMemNode*>(nullptr);
             CodaMemNode* grid_elems[] = {grid_rec};
             const long grid_dims[] = {1};
             return prod.MakeNodeArray(grid_dims, grid_elems);
           }) ||
      trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("significanceOfReferenceTime",
           [&] { return prod.MakeUint8(m.significance_of_reference_time); }) ||
      trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("year", [&] { return prod.MakeUint16(m.year); }) || trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("month", [&] { return prod.MakeUint8(m.month); }) || trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("day", [&] { return prod.MakeUint8(m.day); }) || trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("hour", [&] { return prod.MakeUint8(m.hour); }) || trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("minute", [&] { return prod.MakeUint8(m.minute); }) || trunc) {
    return trunc ? finish() : nullptr;
  }
  if (!add("second", [&] { return prod.MakeUint8(m.second); })) {
    return nullptr;
  }
  return finish();
}

bool SetMessagesRoot(CodaMemProduct& prod, std::vector<CodaMemNode*>& msgs) {
  const long dims[] = {static_cast<long>(msgs.size())};
  CodaMemNode* arr = prod.MakeNodeArray(dims, msgs);
  return arr != nullptr && prod.SetRootNode(arr);
}

std::vector<std::string>* g_captured_warnings = nullptr;

int CaptureWarningHandler(const char* message, va_list ap) {
  char buf[512];
  int n = std::vsnprintf(buf, sizeof(buf), message, ap);
  if (g_captured_warnings != nullptr && n >= 0) {
    g_captured_warnings->emplace_back(buf);
  }
  return n;
}

TEST(HarpIngestEcmwfGribTest, IngestAllGrib1ParametersAndReadValues) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECMWF_GRIB");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod(coda_format_grib);
  ASSERT_TRUE(prod.ok());

  // 3x3 regular lat/lon grid so interior axis interpolation loops run.
  const GridSpec base_grid = {
      .is_gaussian = false,
      .ni = 3,
      .nj = 3,
      .lat_first = 60000,
      .lon_first = 0,
      .lat_last = 40000,
      .lon_last = 20000,
      .di = 10000,
      .dj = 10000,
      .n = 0,
      .scanning_mode = 0,
      .coordinate_values = {10.0, 50.0, 100.0, 0.1, 0.5, 1.0},
  };

  struct ParamEntry {
    uint8_t table;
    uint8_t indicator;
    bool is_profile;
    float base_val;
  };
  constexpr ParamEntry kAllParams[] = {
      {.table = 128, .indicator = 75, .is_profile = true, .base_val = 1.0f},
      {.table = 128, .indicator = 76, .is_profile = true, .base_val = 2.0f},
      {.table = 128, .indicator = 78, .is_profile = false, .base_val = 3.0f},
      {.table = 128, .indicator = 79, .is_profile = false, .base_val = 4.0f},
      {.table = 128, .indicator = 129, .is_profile = false, .base_val = 5.0f},
      {.table = 128, .indicator = 130, .is_profile = true, .base_val = 280.0f},
      {.table = 128, .indicator = 133, .is_profile = true, .base_val = 0.01f},
      {.table = 128, .indicator = 137, .is_profile = false, .base_val = 10.0f},
      {.table = 128, .indicator = 138, .is_profile = true, .base_val = 0.02f},
      {.table = 128, .indicator = 152, .is_profile = false, .base_val = 11.5f},
      {.table = 128, .indicator = 159, .is_profile = false, .base_val = 500.0f},
      {.table = 128, .indicator = 164, .is_profile = false, .base_val = 0.5f},
      {.table = 128, .indicator = 165, .is_profile = false, .base_val = 3.5f},
      {.table = 128, .indicator = 166, .is_profile = false, .base_val = -2.5f},
      {.table = 128, .indicator = 167, .is_profile = false, .base_val = 290.0f},
      {.table = 128, .indicator = 172, .is_profile = false, .base_val = 1.0f},
      {.table = 128, .indicator = 246, .is_profile = true, .base_val = 0.03f},
      {.table = 128, .indicator = 247, .is_profile = true, .base_val = 0.04f},
      {.table = 210, .indicator = 61, .is_profile = true, .base_val = 0.05f},
      {.table = 210, .indicator = 62, .is_profile = true, .base_val = 0.06f},
      {.table = 210, .indicator = 72, .is_profile = false, .base_val = 0.07f},
      {.table = 210, .indicator = 73, .is_profile = false, .base_val = 0.08f},
      {.table = 210, .indicator = 74, .is_profile = false, .base_val = 0.09f},
      {.table = 210, .indicator = 121, .is_profile = true, .base_val = 0.10f},
      {.table = 210, .indicator = 122, .is_profile = true, .base_val = 0.11f},
      {.table = 210, .indicator = 123, .is_profile = true, .base_val = 0.12f},
      {.table = 210, .indicator = 124, .is_profile = true, .base_val = 0.13f},
      {.table = 210, .indicator = 125, .is_profile = false, .base_val = 0.14f},
      {.table = 210, .indicator = 126, .is_profile = false, .base_val = 0.15f},
      {.table = 210, .indicator = 127, .is_profile = false, .base_val = 0.16f},
      {.table = 210, .indicator = 128, .is_profile = false, .base_val = 0.17f},
      {.table = 210, .indicator = 203, .is_profile = true, .base_val = 0.18f},
      {.table = 210, .indicator = 206, .is_profile = false, .base_val = 0.19f},
      {.table = 210, .indicator = 207, .is_profile = false, .base_val = 0.55f},
      {.table = 210, .indicator = 208, .is_profile = false, .base_val = 0.21f},
      {.table = 210, .indicator = 209, .is_profile = false, .base_val = 0.22f},
      {.table = 210, .indicator = 210, .is_profile = false, .base_val = 0.23f},
      {.table = 210, .indicator = 211, .is_profile = false, .base_val = 0.24f},
      {.table = 210, .indicator = 212, .is_profile = false, .base_val = 0.25f},
      {.table = 210, .indicator = 213, .is_profile = false, .base_val = 0.46f},
      {.table = 210, .indicator = 214, .is_profile = false, .base_val = 0.67f},
      {.table = 210, .indicator = 215, .is_profile = false, .base_val = 0.86f},
      {.table = 210, .indicator = 216, .is_profile = false, .base_val = 1.24f},
      {.table = 210, .indicator = 218, .is_profile = false, .base_val = 0.33f},
      {.table = 210, .indicator = 221, .is_profile = false, .base_val = 0.44f},
      {.table = 215, .indicator = 122, .is_profile = false, .base_val = 0.26f},
      {.table = 215, .indicator = 182, .is_profile = true, .base_val = 0.27f},
      {.table = 215, .indicator = 188, .is_profile = true, .base_val = 0.28f},
      {.table = 217, .indicator = 6, .is_profile = true, .base_val = 0.29f},
      {.table = 217, .indicator = 13, .is_profile = true, .base_val = 0.30f},
      {.table = 217, .indicator = 16, .is_profile = true, .base_val = 0.31f},
      {.table = 217, .indicator = 27, .is_profile = true, .base_val = 0.32f},
      {.table = 217, .indicator = 30, .is_profile = true, .base_val = 0.33f},
      {.table = 217, .indicator = 45, .is_profile = true, .base_val = 0.34f},
      {.table = 217, .indicator = 47, .is_profile = true, .base_val = 0.35f},
      {.table = 218, .indicator = 4, .is_profile = false, .base_val = 0.36f},
      {.table = 218, .indicator = 6, .is_profile = false, .base_val = 0.37f},
      {.table = 218, .indicator = 13, .is_profile = false, .base_val = 0.38f},
      {.table = 218, .indicator = 16, .is_profile = false, .base_val = 0.39f},
      {.table = 218, .indicator = 27, .is_profile = false, .base_val = 0.40f},
      {.table = 218, .indicator = 30, .is_profile = false, .base_val = 0.41f},
      {.table = 218, .indicator = 45, .is_profile = false, .base_val = 0.42f},
      {.table = 218, .indicator = 47, .is_profile = false, .base_val = 0.43f},
  };

  std::vector<CodaMemNode*> msgs;
  for (const auto& entry : kAllParams) {
    Grib1MsgSpec msg;
    msg.table2_version = entry.table;
    msg.indicator_of_parameter = entry.indicator;
    msg.grid = base_grid;
    msg.values = std::vector<float>(9, entry.base_val);
    if (entry.is_profile) {
      msg.indicator_of_type_of_level = 109;
      // Leave level 1 absent for crwc (75) to exercise NaN fill on missing
      // level (read_grid_data grid_data_index < 0).
      if (entry.indicator != 75) {
        msg.level = 1;
        msgs.push_back(BuildGrib1UnionMsg(prod, msg));
        ASSERT_NE(nullptr, msgs.back());
      }
      msg.level = 2;
      msg.values[6] = entry.base_val + 1.0f;  // lat=0, lon=0 in HARP order
      msgs.push_back(BuildGrib1UnionMsg(prod, msg));
      ASSERT_NE(nullptr, msgs.back());
    } else {
      // Surface level 0 is accepted and normalized to level 1 (test on tclw).
      msg.indicator_of_type_of_level = 1;
      msg.level = (entry.indicator == 78) ? 0 : 1;
      msgs.push_back(BuildGrib1UnionMsg(prod, msg));
      ASSERT_NE(nullptr, msgs.back());
    }
  }
  ASSERT_TRUE(SetMessagesRoot(prod, msgs));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(1, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(3, res.dimensions[harp_dimension_latitude]);
  EXPECT_EQ(3, res.dimensions[harp_dimension_longitude]);
  EXPECT_EQ(2, res.dimensions[harp_dimension_vertical]);
  EXPECT_EQ(7, res.dimensions[harp_dimension_spectral]);

  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(DoubleNear(40.0, 1e-9), DoubleNear(50.0, 1e-9),
                          DoubleNear(60.0, 1e-9)));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(DoubleNear(0.0, 1e-9), DoubleNear(10.0, 1e-9),
                          DoubleNear(20.0, 1e-9)));
  EXPECT_THAT(res.variables["wavelength"].double_values,
              ElementsAre(335.0, 440.0, 469.0, 550.0, 670.0, 865.0, 1240.0));
  EXPECT_FLOAT_EQ(std::exp(11.5f),
                  res.variables["surface_pressure"].float_values[0]);

  // Verify pressure & pressure_bounds at (lat=0, lon=0, vert=0..1).
  const double ps = static_cast<double>(std::exp(11.5f));
  EXPECT_THAT(
      res.variables["pressure"].float_values[0],
      FloatNear(static_cast<float>(0.5 * (50.0 + 100.0 + 1.5 * ps)), 1e-1f));
  EXPECT_THAT(
      res.variables["pressure"].float_values[1],
      FloatNear(static_cast<float>(0.5 * (10.0 + 50.0 + 0.6 * ps)), 1e-1f));
  EXPECT_THAT(res.variables["pressure_bounds"].float_values[0],
              FloatNear(static_cast<float>(100.0 + 1.0 * ps), 1e-1f));
  EXPECT_THAT(res.variables["pressure_bounds"].float_values[1],
              FloatNear(static_cast<float>(50.0 + 0.5 * ps), 1e-1f));

  // Missing level 1 (TOA, vert=1) for rain_water_mass_mixing_ratio yields NaN.
  EXPECT_FLOAT_EQ(
      2.0f, res.variables["rain_water_mass_mixing_ratio"].float_values[0]);
  EXPECT_TRUE(std::isnan(
      res.variables["rain_water_mass_mixing_ratio"].float_values[1]));

  // Check AOD spectral slice at (lat=0, lon=0): 335..1240 nm.
  const auto& aod = res.variables["aerosol_optical_depth"].float_values;
  EXPECT_THAT(std::vector<float>(aod.begin(), aod.begin() + 7),
              ElementsAre(0.33f, 0.44f, 0.46f, 0.55f, 0.67f, 0.86f, 1.24f));
}

TEST(HarpIngestEcmwfGribTest, IngestRemainingGrib1TablesAndTimeUnits) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECMWF_GRIB");
  ASSERT_NE(nullptr, mod);

  struct Grib1Case {
    uint8_t table;
    std::vector<uint8_t> indicators;
    uint8_t century;
    uint8_t year_of_century;
    uint8_t unit_of_time_range;
    uint8_t p1;
    std::string_view expected_var;
  };
  const Grib1Case cases[] = {
      {.table = 160,
       .indicators = {129, 130, 133, 138, 152, 164, 165, 166, 167, 172},
       .century = 20,
       .year_of_century = 0,
       .unit_of_time_range = 1,
       .p1 = 2,
       .expected_var = "surface_geopotential"},
      {.table = 170,
       .indicators = {129, 130, 133, 138, 164},
       .century = 21,
       .year_of_century = 1,
       .unit_of_time_range = 2,
       .p1 = 1,
       .expected_var = "temperature"},
      {.table = 171,
       .indicators = {172},
       .century = 21,
       .year_of_century = 2,
       .unit_of_time_range = 10,
       .p1 = 1,
       .expected_var = "land_flag"},
      {.table = 174,
       .indicators = {172},
       .century = 21,
       .year_of_century = 3,
       .unit_of_time_range = 11,
       .p1 = 1,
       .expected_var = "land_flag"},
      {.table = 175,
       .indicators = {172},
       .century = 21,
       .year_of_century = 4,
       .unit_of_time_range = 12,
       .p1 = 1,
       .expected_var = "land_flag"},
      {.table = 180,
       .indicators = {129, 130, 133, 137, 138, 164, 165, 166, 167, 172},
       .century = 21,
       .year_of_century = 5,
       .unit_of_time_range = 13,
       .p1 = 30,
       .expected_var = "H2O_column_density"},
      {.table = 190,
       .indicators = {129, 130, 133, 138, 164, 165, 166, 167, 172},
       .century = 21,
       .year_of_century = 6,
       .unit_of_time_range = 0,
       .p1 = 0,
       .expected_var = "cloud_fraction"},
      {.table = 217,
       .indicators = {4},
       .century = 21,
       .year_of_century = 7,
       .unit_of_time_range = 0,
       .p1 = 0,
       .expected_var = "CH4_mass_mixing_ratio_dry_air"},
  };

  for (const auto& tc : cases) {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    std::vector<CodaMemNode*> msgs;
    for (uint8_t ind : tc.indicators) {
      Grib1MsgSpec msg;
      msg.century = tc.century;
      msg.year_of_century = tc.year_of_century;
      msg.unit_of_time_range = tc.unit_of_time_range;
      msg.p1 = tc.p1;
      msg.table2_version = tc.table;
      msg.indicator_of_parameter = ind;
      msgs.push_back(BuildGrib1UnionMsg(prod, msg));
      ASSERT_NE(nullptr, msgs.back());
    }
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status)
        << "table=" << static_cast<int>(tc.table);
    EXPECT_TRUE(res.variables.at(std::string(tc.expected_var)).included);
  }
}

TEST(HarpIngestEcmwfGribTest, IngestAllGrib2ParameterMappingsAndTimeUnits) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECMWF_GRIB");
  ASSERT_NE(nullptr, mod);

  // Exercise all GRIB2 discipline/category/number/constituentType mappings
  // across non-colliding groups, plus all valid indicatorOfUnitOfTimeRange and
  // significanceOfReferenceTime values.
  struct Grib2Item {
    uint8_t disc;
    uint8_t cat;
    uint8_t num;
    std::optional<uint16_t> ctype;
    bool is_profile;
  };

  // Batch A: discipline 0 and 2, plus discipline 192 params not colliding.
  const std::vector<Grib2Item> batch_a = {
      {0, 0, 0, std::nullopt, true},    // t
      {0, 1, 0, std::nullopt, true},    // q
      {0, 1, 69, std::nullopt, false},  // tclw
      {0, 1, 70, std::nullopt, false},  // tciw
      {0, 1, 83, std::nullopt, true},   // clwc
      {0, 1, 84, std::nullopt, true},   // ciwc
      {0, 1, 85, std::nullopt, true},   // crwc
      {0, 1, 86, std::nullopt, true},   // cswc
      {0, 2, 2, std::nullopt, false},   // 10u
      {0, 2, 3, std::nullopt, false},   // 10v
      {0, 2, 12, std::nullopt, true},   // vo
      {0, 3, 4, std::nullopt, false},   // z
      {0, 3, 25, std::nullopt, false},  // lnsp
      {0, 20, 2, 0, true},              // go3
      {0, 20, 2, 2, true},              // ch4
      {0, 20, 2, 3, true},              // co2
      {0, 20, 2, 4, true},              // co
      {0, 20, 2, 5, true},              // no2
      {0, 20, 2, 7, true},              // hcho
      {0, 20, 2, 8, true},              // so2
      {0, 20, 2, 11, true},             // no
      {0, 20, 2, 17, true},             // hno3
      {2, 0, 0, std::nullopt, false},   // lsm
  };

  // Batch B: all discipline 192 entries.
  const std::vector<Grib2Item> batch_b = {
      {192, 128, 78, std::nullopt, false},
      {192, 128, 79, std::nullopt, false},
      {192, 128, 137, std::nullopt, false},
      {192, 128, 159, std::nullopt, false},
      {192, 128, 164, std::nullopt, false},
      {192, 210, 61, std::nullopt, true},
      {192, 210, 62, std::nullopt, true},
      {192, 210, 72, std::nullopt, false},
      {192, 210, 73, std::nullopt, false},
      {192, 210, 74, std::nullopt, false},
      {192, 210, 121, std::nullopt, true},
      {192, 210, 122, std::nullopt, true},
      {192, 210, 123, std::nullopt, true},
      {192, 210, 124, std::nullopt, true},
      {192, 210, 125, std::nullopt, false},
      {192, 210, 126, std::nullopt, false},
      {192, 210, 127, std::nullopt, false},
      {192, 210, 128, std::nullopt, false},
      {192, 210, 203, std::nullopt, true},
      {192, 210, 206, std::nullopt, false},
      {192, 210, 207, std::nullopt, false},
      {192, 210, 208, std::nullopt, false},
      {192, 210, 209, std::nullopt, false},
      {192, 210, 210, std::nullopt, false},
      {192, 210, 211, std::nullopt, false},
      {192, 210, 212, std::nullopt, false},
      {192, 210, 213, std::nullopt, false},
      {192, 210, 214, std::nullopt, false},
      {192, 210, 215, std::nullopt, false},
      {192, 210, 216, std::nullopt, false},
      {192, 210, 218, std::nullopt, false},
      {192, 210, 221, std::nullopt, false},
      {192, 215, 122, std::nullopt, false},
      {192, 215, 182, std::nullopt, true},
      {192, 215, 188, std::nullopt, true},
      {192, 217, 13, std::nullopt, true},
      {192, 217, 16, std::nullopt, true},
      {192, 217, 27, std::nullopt, true},
      {192, 217, 30, std::nullopt, true},
      {192, 217, 45, std::nullopt, true},
      {192, 217, 47, std::nullopt, true},
      {192, 218, 4, std::nullopt, false},
      {192, 218, 6, std::nullopt, false},
      {192, 218, 13, std::nullopt, false},
      {192, 218, 16, std::nullopt, false},
      {192, 218, 27, std::nullopt, false},
      {192, 218, 30, std::nullopt, false},
      {192, 218, 45, std::nullopt, false},
      {192, 218, 47, std::nullopt, false},
  };

  for (const auto& batch : {batch_a, batch_b}) {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    std::vector<CodaMemNode*> msgs;
    for (const auto& item : batch) {
      Grib2MsgSpec msg;
      msg.discipline = item.disc;
      Grib2DataSpec d;
      d.parameter_category = item.cat;
      d.parameter_number = item.num;
      d.constituent_type = item.ctype;
      if (item.is_profile) {
        d.first_fixed_surface = 2.0;
        d.coordinate_values = {10.0, 50.0, 100.0, 0.1, 0.5, 1.0};
      }
      msg.data.push_back(d);
      msgs.push_back(BuildGrib2UnionMsg(prod, msg));
      ASSERT_NE(nullptr, msgs.back());
    }
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
  }

  // Remaining GRIB2 (192, 217, 4) for ch4 and (192, 217, 6) for hno3 plus all
  // time units & significanceOfReferenceTime values.
  struct TimeCase {
    uint8_t sig;
    uint8_t unit;
    uint32_t forecast;
    uint8_t param_num;
  };
  constexpr TimeCase kTimeCases[] = {
      {.sig = 0, .unit = 0, .forecast = 10, .param_num = 6},
      {.sig = 1, .unit = 1, .forecast = 2, .param_num = 4},
      {.sig = 2, .unit = 2, .forecast = 1, .param_num = 6},
      {.sig = 3, .unit = 10, .forecast = 1, .param_num = 6},
      {.sig = 0, .unit = 11, .forecast = 1, .param_num = 6},
      {.sig = 0, .unit = 12, .forecast = 1, .param_num = 6},
      {.sig = 0, .unit = 13, .forecast = 30, .param_num = 6},
  };
  for (const auto& tc : kTimeCases) {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib2MsgSpec msg;
    msg.significance_of_reference_time = tc.sig;
    msg.discipline = 192;
    Grib2DataSpec d;
    d.parameter_category = 217;
    d.parameter_number = tc.param_num;
    d.unit_of_time_range = tc.unit;
    d.forecast_time = tc.forecast;
    msg.data.push_back(d);
    std::vector<CodaMemNode*> msgs = {BuildGrib2UnionMsg(prod, msg)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
  }
}

TEST(HarpIngestEcmwfGribTest, GaussianGridsAndMixedGrib1Grib2Grid) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECMWF_GRIB");
  ASSERT_NE(nullptr, mod);

  // 1. Gaussian grid with N = 52 (Nj = 104, Ni = 1) to exercise i >= 50 in
  // gauss_first_guess and grib_get_gaussian_latitudes.
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec msg;
    msg.grid = {
        .is_gaussian = true,
        .ni = 1,
        .nj = 104,
        .lat_first = 88000,
        .lon_first = 0,
        .lat_last = -88000,
        .lon_last = 0,
        .di = 0,
        .dj = 0,
        .n = 52,
        .scanning_mode = 0,
    };
    msg.values = std::vector<float>(104, 1.5f);
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, msg)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(104, res.dimensions[harp_dimension_latitude]);
    // Symmetric North/South latitudes.
    EXPECT_DOUBLE_EQ(-res.variables["latitude"].double_values.front(),
                     res.variables["latitude"].double_values.back());
  }

  // 2. Mixed GRIB1 (less precise) followed by GRIB2 (more precise, within
  // 1000 tolerance) upgrades grid coordinates to GRIB2 precision.
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m1;
    m1.table2_version = 128;
    m1.indicator_of_parameter = 129;
    m1.grid = {
        .is_gaussian = false,
        .ni = 3,
        .nj = 3,
        .lat_first = 60000,
        .lon_first = 0,
        .lat_last = 40000,
        .lon_last = 20000,
        .di = 10000,
        .dj = 10000,
    };
    m1.values = std::vector<float>(9, 1.0f);

    Grib2MsgSpec m2;
    m2.discipline = 0;
    m2.grid = {
        .is_gaussian = false,
        .ni = 3,
        .nj = 3,
        .lat_first = 60000400,
        .lon_first = 500,
        .lat_last = 40000400,
        .lon_last = 20000500,
        .di = 10000000,
        .dj = 10000000,
    };
    Grib2DataSpec d;
    d.parameter_category = 1;
    d.parameter_number = 69;  // tclw
    d.values = std::vector<float>(9, 2.0f);
    m2.data.push_back(d);

    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m1),
                                      BuildGrib2UnionMsg(prod, m2)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["latitude"].double_values[0],
                DoubleNear(40.0004, 1e-6));
    EXPECT_THAT(res.variables["longitude"].double_values[0],
                DoubleNear(0.0005, 1e-6));
  }

  // 3. Mixed GRIB2 Gaussian followed by GRIB1 Gaussian (covers !is_gaussian
  // false branch and grib_version == 1 branch in mixed-version check).
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib2MsgSpec m2;
    m2.discipline = 0;
    m2.grid = {
        .is_gaussian = true,
        .ni = 2,
        .nj = 4,
        .lat_first = 60000000,
        .lon_first = 0,
        .lat_last = -60000000,
        .lon_last = 10000000,
        .di = 10000000,
        .n = 2,
    };
    Grib2DataSpec d;
    d.parameter_category = 1;
    d.parameter_number = 69;
    d.values = std::vector<float>(8, 2.0f);
    m2.data.push_back(d);

    Grib1MsgSpec m1;
    m1.grid = {
        .is_gaussian = true,
        .ni = 2,
        .nj = 4,
        .lat_first = 60000,
        .lon_first = 0,
        .lat_last = -60000,
        .lon_last = 10000,
        .di = 10000,
        .n = 2,
    };
    m1.values = std::vector<float>(8, 1.0f);

    std::vector<CodaMemNode*> msgs = {BuildGrib2UnionMsg(prod, m2),
                                      BuildGrib1UnionMsg(prod, m1)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
  }
}

TEST(HarpIngestEcmwfGribTest, UnknownParametersEmitSingleWarningPerType) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECMWF_GRIB");
  ASSERT_NE(nullptr, mod);

  int (*prev_handler)(const char*, va_list) = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_get_warning_handler(&prev_handler));
  auto restore_handler = absl::MakeCleanup([&] {
    harp_set_warning_handler(prev_handler);
    g_captured_warnings = nullptr;
  });

  std::vector<std::string> warnings;
  g_captured_warnings = &warnings;
  ASSERT_EQ(HARP_SUCCESS, harp_set_warning_handler(CaptureWarningHandler));

  CodaMemProduct prod(coda_format_grib);
  ASSERT_TRUE(prod.ok());

  // Duplicate unknown GRIB1 (99, 1) and duplicate unknown GRIB2 (0, 99, 1)
  // should emit exactly one warning for each.
  Grib1MsgSpec u1;
  u1.table2_version = 99;
  u1.indicator_of_parameter = 1;

  Grib2MsgSpec u2;
  u2.discipline = 0;
  Grib2DataSpec ud;
  ud.parameter_category = 99;
  ud.parameter_number = 1;
  u2.data.push_back(ud);

  std::vector<CodaMemNode*> msgs = {
      BuildGrib1UnionMsg(prod, u1), BuildGrib1UnionMsg(prod, u1),
      BuildGrib2UnionMsg(prod, u2), BuildGrib2UnionMsg(prod, u2)};
  ASSERT_TRUE(SetMessagesRoot(prod, msgs));
  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  ASSERT_EQ(2u, warnings.size());
  EXPECT_THAT(warnings[0], HasSubstr("unsupported GRIB1 parameter"));
  EXPECT_THAT(warnings[1], HasSubstr("unsupported GRIB2 parameter"));
}

TEST(HarpIngestEcmwfGribTest, IngestionErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto clear_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });
  const harp_ingestion_module* mod = FindIngestionModule("ECMWF_GRIB");
  ASSERT_NE(nullptr, mod);

  // 1. Root is not an array.
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Root is empty array.
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    const long empty_dims[] = {0};
    CodaMemNode* sample = prod.MakeRecord();
    ASSERT_TRUE(prod.SetRootNode(prod.MakeNodeArray(empty_dims, {}, sample)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Root element is not a union.
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    std::vector<CodaMemNode*> msgs = {prod.MakeRecord()};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. GRIB2 missing data, non-array data, and empty data array.
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    CodaMemNode* m = prod.MakeRecord();
    std::vector<CodaMemNode*> msgs = {prod.MakeUnion(kGribUnionFields, 1, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    CodaMemNode* m = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(m, "data", prod.MakeInt32(1)));
    std::vector<CodaMemNode*> msgs = {prod.MakeUnion(kGribUnionFields, 1, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    CodaMemNode* m = prod.MakeRecord();
    const long empty_dims[] = {0};
    ASSERT_TRUE(prod.AddField(
        m, "data", prod.MakeNodeArray(empty_dims, {}, prod.MakeRecord())));
    std::vector<CodaMemNode*> msgs = {prod.MakeUnion(kGribUnionFields, 1, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 5. Unsupported GRIB1 unitOfTimeRange, invalid date, wrong field types.
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m;
    m.unit_of_time_range = 99;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m;
    m.month = 99;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 6. Unsupported GRIB2 significanceOfReferenceTime and
  // indicatorOfUnitOfTimeRange.
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib2MsgSpec m;
    m.significance_of_reference_time = 9;
    m.data.push_back(Grib2DataSpec{});
    std::vector<CodaMemNode*> msgs = {BuildGrib2UnionMsg(prod, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib2MsgSpec m;
    Grib2DataSpec d;
    d.unit_of_time_range = 99;
    m.data.push_back(d);
    std::vector<CodaMemNode*> msgs = {BuildGrib2UnionMsg(prod, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 7. Grid validation errors: reduced Gaussian, scanningMode, lat/lon order,
  // Gaussian N mismatch, mismatched grid sizes/types/coords across messages.
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m;
    m.grid.ni = 65535;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib2MsgSpec m;
    m.grid.nj = 4294967295U;
    m.data.push_back(Grib2DataSpec{});
    std::vector<CodaMemNode*> msgs = {BuildGrib2UnionMsg(prod, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m;
    m.grid.scanning_mode = 64;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m;
    m.grid.lon_first = 30000;
    m.grid.lon_last = 10000;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m;
    m.grid.lat_first = 10000;
    m.grid.lat_last = 30000;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m;
    m.grid.is_gaussian = true;
    m.grid.n = 99;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // Multi-message grid mismatches (size, type, same-version lon/lat,
  // mixed-version lon/lat).
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m1, m2;
    m2.indicator_of_parameter = 137;
    m2.grid.ni = 3;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m1),
                                      BuildGrib1UnionMsg(prod, m2)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m1, m2;
    m2.indicator_of_parameter = 137;
    m2.grid.is_gaussian = true;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m1),
                                      BuildGrib1UnionMsg(prod, m2)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m1, m2;
    m2.indicator_of_parameter = 137;
    m2.grid.lon_last = 25000;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m1),
                                      BuildGrib1UnionMsg(prod, m2)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m1, m2;
    m2.indicator_of_parameter = 137;
    m2.grid.lat_last = 35000;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m1),
                                      BuildGrib1UnionMsg(prod, m2)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m1;
    Grib2MsgSpec m2;
    m2.grid.lon_first = 10005000;  // diff > 1000
    m2.data.push_back(Grib2DataSpec{});
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m1),
                                      BuildGrib2UnionMsg(prod, m2)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m1;
    Grib2MsgSpec m2;
    m2.grid.lat_first = 50005000;  // diff > 1000
    m2.data.push_back(Grib2DataSpec{});
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m1),
                                      BuildGrib2UnionMsg(prod, m2)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }

  // 8. Time mismatch, level type mismatch, coordinateValues length mismatch,
  // invalid levels, duplicate parameter+level.
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m1, m2;
    m2.hour = 13;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m1),
                                      BuildGrib1UnionMsg(prod, m2)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib2MsgSpec m;
    Grib2DataSpec d1, d2;
    d2.forecast_time = 2;
    m.data = {d1, d2};
    std::vector<CodaMemNode*> msgs = {BuildGrib2UnionMsg(prod, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m;
    m.indicator_of_type_of_level = 99;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib2MsgSpec m;
    Grib2DataSpec d;
    d.type_of_first_fixed_surface = 99;
    m.data.push_back(d);
    std::vector<CodaMemNode*> msgs = {BuildGrib2UnionMsg(prod, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m1, m2;
    m1.indicator_of_parameter = 130;
    m1.indicator_of_type_of_level = 109;
    m1.level = 2;
    m1.grid.coordinate_values = {10.0, 50.0, 100.0, 0.1, 0.5, 1.0};
    m2.indicator_of_parameter = 133;
    m2.indicator_of_type_of_level = 109;
    m2.level = 2;
    m2.grid.coordinate_values = {10.0, 20.0, 50.0, 100.0, 0.1, 0.2, 0.5, 1.0};
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m1),
                                      BuildGrib1UnionMsg(prod, m2)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib2MsgSpec m1, m2;
    Grib2DataSpec d1, d2;
    d1.parameter_category = 0;
    d1.parameter_number = 0;
    d1.first_fixed_surface = 2.0;
    d1.coordinate_values = {10.0, 50.0, 100.0, 0.1, 0.5, 1.0};
    d2.parameter_category = 1;
    d2.parameter_number = 0;
    d2.first_fixed_surface = 2.0;
    d2.coordinate_values = {10.0, 20.0, 50.0, 100.0, 0.1, 0.2, 0.5, 1.0};
    m1.data = {d1};
    m2.data = {d2};
    std::vector<CodaMemNode*> msgs = {BuildGrib2UnionMsg(prod, m1),
                                      BuildGrib2UnionMsg(prod, m2)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m1, m2;
    m1.indicator_of_parameter = 130;
    m1.indicator_of_type_of_level = 109;
    m1.level = 2;
    m1.grid.coordinate_values = {10.0, 50.0, 100.0, 0.1, 0.5, 1.0};
    m2.indicator_of_parameter = 133;
    m2.indicator_of_type_of_level = 109;
    m2.level = 5;  // > num_grib_levels (2)
    m2.grid.coordinate_values = {10.0, 50.0, 100.0, 0.1, 0.5, 1.0};
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m1),
                                      BuildGrib1UnionMsg(prod, m2)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m;
    m.indicator_of_parameter = 129;  // surface parameter
    m.level = 2;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m;
    m.indicator_of_parameter = 129;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m),
                                      BuildGrib1UnionMsg(prod, m)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }

  // 9. Unsupported GRIB1/GRIB2 grid definitions.
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    CodaMemNode* msg = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(msg, "centuryOfReferenceTimeOfData", prod.MakeUint8(21)));
    ASSERT_TRUE(prod.AddField(msg, "yearOfCentury", prod.MakeUint8(20)));
    ASSERT_TRUE(prod.AddField(msg, "month", prod.MakeUint8(5)));
    ASSERT_TRUE(prod.AddField(msg, "day", prod.MakeUint8(15)));
    ASSERT_TRUE(prod.AddField(msg, "hour", prod.MakeUint8(12)));
    ASSERT_TRUE(prod.AddField(msg, "minute", prod.MakeUint8(0)));
    ASSERT_TRUE(prod.AddField(msg, "unitOfTimeRange", prod.MakeUint8(0)));
    ASSERT_TRUE(prod.AddField(msg, "P1", prod.MakeUint8(0)));
    CodaMemNode* grid = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(grid, "dataRepresentationType", prod.MakeUint8(99)));
    ASSERT_TRUE(prod.AddField(msg, "grid", grid));
    std::vector<CodaMemNode*> msgs = {prod.MakeUnion(kGribUnionFields, 0, msg)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    CodaMemNode* msg = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(msg, "significanceOfReferenceTime", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(msg, "year", prod.MakeUint16(2020)));
    ASSERT_TRUE(prod.AddField(msg, "month", prod.MakeUint8(5)));
    ASSERT_TRUE(prod.AddField(msg, "day", prod.MakeUint8(15)));
    ASSERT_TRUE(prod.AddField(msg, "hour", prod.MakeUint8(12)));
    ASSERT_TRUE(prod.AddField(msg, "minute", prod.MakeUint8(0)));
    ASSERT_TRUE(prod.AddField(msg, "second", prod.MakeUint8(0)));
    ASSERT_TRUE(prod.AddField(msg, "discipline", prod.MakeUint8(0)));
    CodaMemNode* grid_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(grid_rec, "gridDefinitionTemplateNumber",
                              prod.MakeUint16(99)));
    CodaMemNode* grid_elems[] = {grid_rec};
    const long grid_dims[] = {1};
    ASSERT_TRUE(
        prod.AddField(msg, "grid", prod.MakeNodeArray(grid_dims, grid_elems)));
    CodaMemNode* drec = prod.MakeRecord();
    CodaMemNode* data_elems[] = {drec};
    ASSERT_TRUE(
        prod.AddField(msg, "data", prod.MakeNodeArray(grid_dims, data_elems)));
    std::vector<CodaMemNode*> msgs = {prod.MakeUnion(kGribUnionFields, 1, msg)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }

  // 10. GRIB2 empty grid array and missing coordinateValues / values.
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    CodaMemNode* msg = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(msg, "significanceOfReferenceTime", prod.MakeUint8(1)));
    ASSERT_TRUE(prod.AddField(msg, "year", prod.MakeUint16(2020)));
    ASSERT_TRUE(prod.AddField(msg, "month", prod.MakeUint8(5)));
    ASSERT_TRUE(prod.AddField(msg, "day", prod.MakeUint8(15)));
    ASSERT_TRUE(prod.AddField(msg, "hour", prod.MakeUint8(12)));
    ASSERT_TRUE(prod.AddField(msg, "minute", prod.MakeUint8(0)));
    ASSERT_TRUE(prod.AddField(msg, "second", prod.MakeUint8(0)));
    ASSERT_TRUE(prod.AddField(msg, "discipline", prod.MakeUint8(0)));
    const long empty_dims[] = {0};
    ASSERT_TRUE(prod.AddField(
        msg, "grid", prod.MakeNodeArray(empty_dims, {}, prod.MakeRecord())));
    const long one_dims[] = {1};
    CodaMemNode* data_elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(
        prod.AddField(msg, "data", prod.MakeNodeArray(one_dims, data_elems)));
    std::vector<CodaMemNode*> msgs = {prod.MakeUnion(kGribUnionFields, 1, msg)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }

  // 11. Read-time error when values array is shorter than Ni * Nj.
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m_lnsp;
    m_lnsp.indicator_of_parameter = 152;
    m_lnsp.values = {1.0f};  // Expected 4 floats for 2x2 grid.

    Grib1MsgSpec m_t;
    m_t.indicator_of_parameter = 130;
    m_t.indicator_of_type_of_level = 109;
    m_t.level = 2;
    m_t.grid.coordinate_values = {10.0, 50.0, 100.0, 0.1, 0.5, 1.0};
    m_t.values = {1.0f};

    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(prod, m_lnsp),
                                      BuildGrib1UnionMsg(prod, m_t)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["surface_pressure"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1, res.variables["pressure_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["temperature"].read_status);
  }
}

TEST(HarpIngestEcmwfGribTest, UnknownTableAndDisciplineSwitches) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ECMWF_GRIB");
  ASSERT_NE(nullptr, mod);

  int (*prev_handler)(const char*, va_list) = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_get_warning_handler(&prev_handler));
  auto restore_handler =
      absl::MakeCleanup([&] { harp_set_warning_handler(prev_handler); });
  ASSERT_EQ(HARP_SUCCESS, harp_set_warning_handler(nullptr));

  // Unknown indicatorOfParameter in each supported GRIB1 table2Version.
  constexpr uint8_t kGrib1Tables[] = {128, 160, 170, 171, 174, 175,
                                      180, 190, 210, 215, 217, 218};
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    std::vector<CodaMemNode*> msgs;
    for (uint8_t tbl : kGrib1Tables) {
      Grib1MsgSpec m;
      m.table2_version = tbl;
      m.indicator_of_parameter = 254;
      msgs.push_back(BuildGrib1UnionMsg(prod, m));
      ASSERT_NE(nullptr, msgs.back());
    }
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    ASSERT_EQ(HARP_SUCCESS,
              ExerciseIngestionModule(mod, prod.product()).status);
  }

  // Unknown parameterNumber / category / constituentType / omitted
  // constituentType in each GRIB2 discipline/category branch.
  struct Grib2UnknownCase {
    uint8_t disc;
    uint8_t cat;
    uint8_t num;
    std::optional<uint16_t> ctype;
    bool omit_ctype;
  };
  constexpr Grib2UnknownCase kGrib2Unknowns[] = {
      {.disc = 0,
       .cat = 0,
       .num = 99,
       .ctype = std::nullopt,
       .omit_ctype = true},
      {.disc = 0,
       .cat = 1,
       .num = 99,
       .ctype = std::nullopt,
       .omit_ctype = false},
      {.disc = 0,
       .cat = 2,
       .num = 99,
       .ctype = std::nullopt,
       .omit_ctype = false},
      {.disc = 0,
       .cat = 3,
       .num = 99,
       .ctype = std::nullopt,
       .omit_ctype = false},
      {.disc = 0, .cat = 20, .num = 99, .ctype = 0, .omit_ctype = false},
      {.disc = 0, .cat = 20, .num = 2, .ctype = 999, .omit_ctype = false},
      {.disc = 0,
       .cat = 99,
       .num = 1,
       .ctype = std::nullopt,
       .omit_ctype = false},
      {.disc = 2,
       .cat = 0,
       .num = 99,
       .ctype = std::nullopt,
       .omit_ctype = false},
      {.disc = 2,
       .cat = 99,
       .num = 1,
       .ctype = std::nullopt,
       .omit_ctype = false},
      {.disc = 192,
       .cat = 128,
       .num = 99,
       .ctype = std::nullopt,
       .omit_ctype = false},
      {.disc = 192,
       .cat = 210,
       .num = 99,
       .ctype = std::nullopt,
       .omit_ctype = false},
      {.disc = 192,
       .cat = 215,
       .num = 99,
       .ctype = std::nullopt,
       .omit_ctype = false},
      {.disc = 192,
       .cat = 217,
       .num = 99,
       .ctype = std::nullopt,
       .omit_ctype = false},
      {.disc = 192,
       .cat = 218,
       .num = 99,
       .ctype = std::nullopt,
       .omit_ctype = false},
      {.disc = 192,
       .cat = 99,
       .num = 1,
       .ctype = std::nullopt,
       .omit_ctype = false},
      {.disc = 99,
       .cat = 0,
       .num = 0,
       .ctype = std::nullopt,
       .omit_ctype = false},
  };
  for (const auto& tc : kGrib2Unknowns) {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib2MsgSpec m;
    m.discipline = tc.disc;
    Grib2DataSpec d;
    d.parameter_category = tc.cat;
    d.parameter_number = tc.num;
    d.constituent_type = tc.ctype;
    m.data.push_back(d);
    std::vector<CodaMemNode*> msgs = {
        BuildGrib2UnionMsg(prod, m, {}, tc.omit_ctype)};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    ASSERT_EQ(HARP_SUCCESS,
              ExerciseIngestionModule(mod, prod.product()).status);
  }
}

TEST(HarpIngestEcmwfGribTest, ReadAodFailureForEachWavelength) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto clear_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });
  const harp_ingestion_module* mod = FindIngestionModule("ECMWF_GRIB");
  ASSERT_NE(nullptr, mod);

  constexpr uint8_t kAodIndicators[] = {218, 221, 213, 207, 214, 215, 216};
  for (size_t fail_idx = 0; fail_idx < std::size(kAodIndicators); ++fail_idx) {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    std::vector<CodaMemNode*> msgs;
    for (size_t i = 0; i <= fail_idx; ++i) {
      Grib1MsgSpec m;
      m.table2_version = 210;
      m.indicator_of_parameter = kAodIndicators[i];
      if (i == fail_idx) {
        m.values = {1.0f};  // Too short for 2x2 grid.
      }
      msgs.push_back(BuildGrib1UnionMsg(prod, m));
      ASSERT_NE(nullptr, msgs.back());
    }
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status) << "fail_idx=" << fail_idx;
  }
}

TEST(HarpIngestEcmwfGribTest, StructuralMutationCodaErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto clear_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });
  const harp_ingestion_module* mod = FindIngestionModule("ECMWF_GRIB");
  ASSERT_NE(nullptr, mod);

  // 1. GRIB1 message field mutations (omit / truncate / bad type).
  constexpr std::string_view kGrib1ByName[] = {
      "centuryOfReferenceTimeOfData",
      "table2Version",
      "indicatorOfParameter",
      "indicatorOfTypeOfLevel",
      "level",
      "grid",
      "data",
  };
  constexpr std::string_view kGrib1Seq[] = {
      "unitOfTimeRange", "P1", "yearOfCentury", "month", "day", "hour",
      "minute",
  };
  for (std::string_view field : kGrib1ByName) {
    for (MutMode mode : {MutMode::kOmitOne, MutMode::kBadType}) {
      CodaMemProduct prod(coda_format_grib);
      ASSERT_TRUE(prod.ok());
      Grib1MsgSpec m;
      m.indicator_of_parameter = 130;
      m.indicator_of_type_of_level = 109;
      m.level = 2;
      m.grid.coordinate_values = {10.0, 50.0, 100.0, 0.1, 0.5, 1.0};
      std::vector<CodaMemNode*> msgs = {
          BuildGrib1UnionMsg(prod, m, {MutScope::kMsg, mode, field})};
      ASSERT_TRUE(SetMessagesRoot(prod, msgs));
      EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status)
          << "grib1 msg field=" << field;
    }
  }
  for (std::string_view field : kGrib1Seq) {
    for (MutMode mode : {MutMode::kTruncateBefore, MutMode::kBadType}) {
      CodaMemProduct prod(coda_format_grib);
      ASSERT_TRUE(prod.ok());
      Grib1MsgSpec m;
      std::vector<CodaMemNode*> msgs = {
          BuildGrib1UnionMsg(prod, m, {MutScope::kMsg, mode, field})};
      ASSERT_TRUE(SetMessagesRoot(prod, msgs));
      EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status)
          << "grib1 seq field=" << field;
    }
  }
  // GRIB1 data/values omitted.
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m;
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(
        prod, m, {MutScope::kData, MutMode::kOmitOne, "values"})};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }

  // 2. GRIB1 regular & Gaussian grid field mutations + coordinateValues.
  constexpr std::string_view kGrib1GridFields[] = {
      "dataRepresentationType",
      "Ni",
      "Nj",
      "latitudeOfFirstGridPoint",
      "longitudeOfFirstGridPoint",
      "latitudeOfLastGridPoint",
      "longitudeOfLastGridPoint",
      "iDirectionIncrement",
      "jDirectionIncrement",
      "scanningMode",
      "coordinateValues",
  };
  for (std::string_view field : kGrib1GridFields) {
    for (MutMode mode : {MutMode::kTruncateBefore, MutMode::kBadType}) {
      CodaMemProduct prod(coda_format_grib);
      ASSERT_TRUE(prod.ok());
      Grib1MsgSpec m;
      m.indicator_of_parameter = 130;
      m.indicator_of_type_of_level = 109;
      m.level = 2;
      m.grid.coordinate_values = {10.0, 50.0, 100.0, 0.1, 0.5, 1.0};
      std::vector<CodaMemNode*> msgs = {
          BuildGrib1UnionMsg(prod, m, {MutScope::kGrid, mode, field})};
      ASSERT_TRUE(SetMessagesRoot(prod, msgs));
      EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status)
          << "grib1 grid field=" << field;
    }
  }
  // GRIB1 Gaussian N missing / bad type, plus scalar coordinateValues
  // (triggers get_num_elements failure before read_double_array).
  for (MutMode mode : {MutMode::kTruncateBefore, MutMode::kBadType}) {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m;
    m.grid.is_gaussian = true;
    m.grid.nj = 4;
    m.grid.n = 2;
    m.values = std::vector<float>(8, 1.0f);
    std::vector<CodaMemNode*> msgs = {
        BuildGrib1UnionMsg(prod, m, {MutScope::kGrid, mode, "N"})};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib1MsgSpec m;
    m.indicator_of_parameter = 130;
    m.indicator_of_type_of_level = 109;
    m.level = 2;
    m.grid.coordinate_values = {10.0, 50.0, 100.0, 0.1, 0.5, 1.0};
    std::vector<CodaMemNode*> msgs = {BuildGrib1UnionMsg(
        prod, m, {MutScope::kGrid, MutMode::kBadScalar, "coordinateValues"})};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib2MsgSpec m;
    Grib2DataSpec d;
    d.first_fixed_surface = 2.0;
    d.coordinate_values = {10.0, 50.0, 100.0, 0.1, 0.5, 1.0};
    m.data.push_back(d);
    std::vector<CodaMemNode*> msgs = {BuildGrib2UnionMsg(
        prod, m, {MutScope::kData, MutMode::kBadScalar, "coordinateValues"})};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }

  // 3. GRIB2 message, grid, and data field mutations.
  constexpr std::string_view kGrib2MsgFields[] = {
      "discipline", "grid",   "significanceOfReferenceTime",
      "year",       "month",  "day",
      "hour",       "minute", "second",
  };
  for (std::string_view field : kGrib2MsgFields) {
    for (MutMode mode : {MutMode::kTruncateBefore, MutMode::kBadType}) {
      CodaMemProduct prod(coda_format_grib);
      ASSERT_TRUE(prod.ok());
      Grib2MsgSpec m;
      m.data.push_back(Grib2DataSpec{});
      std::vector<CodaMemNode*> msgs = {
          BuildGrib2UnionMsg(prod, m, {MutScope::kMsg, mode, field})};
      ASSERT_TRUE(SetMessagesRoot(prod, msgs));
      EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status)
          << "grib2 msg field=" << field;
    }
  }
  constexpr std::string_view kGrib2GridFields[] = {
      "gridDefinitionTemplateNumber",
      "Ni",
      "Nj",
      "latitudeOfFirstGridPoint",
      "longitudeOfFirstGridPoint",
      "latitudeOfLastGridPoint",
      "longitudeOfLastGridPoint",
      "iDirectionIncrement",
      "jDirectionIncrement",
      "scanningMode",
  };
  for (std::string_view field : kGrib2GridFields) {
    for (MutMode mode : {MutMode::kTruncateBefore, MutMode::kBadType}) {
      CodaMemProduct prod(coda_format_grib);
      ASSERT_TRUE(prod.ok());
      Grib2MsgSpec m;
      m.data.push_back(Grib2DataSpec{});
      std::vector<CodaMemNode*> msgs = {
          BuildGrib2UnionMsg(prod, m, {MutScope::kGrid, mode, field})};
      ASSERT_TRUE(SetMessagesRoot(prod, msgs));
      EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status)
          << "grib2 grid field=" << field;
    }
  }
  constexpr std::string_view kGrib2DataFields[] = {
      "parameterCategory",          "parameterNumber",
      "indicatorOfUnitOfTimeRange", "forecastTime",
      "typeOfFirstFixedSurface",    "firstFixedSurface",
      "coordinateValues",           "values",
  };
  for (std::string_view field : kGrib2DataFields) {
    for (MutMode mode : {MutMode::kTruncateBefore, MutMode::kBadType}) {
      if (field == "values" && mode == MutMode::kBadType) continue;
      CodaMemProduct prod(coda_format_grib);
      ASSERT_TRUE(prod.ok());
      Grib2MsgSpec m;
      Grib2DataSpec d;
      d.first_fixed_surface = 2.0;
      d.coordinate_values = {10.0, 50.0, 100.0, 0.1, 0.5, 1.0};
      m.data.push_back(d);
      std::vector<CodaMemNode*> msgs = {
          BuildGrib2UnionMsg(prod, m, {MutScope::kData, mode, field})};
      ASSERT_TRUE(SetMessagesRoot(prod, msgs));
      EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status)
          << "grib2 data field=" << field;
    }
  }
  // GRIB2 constituentType bad type.
  {
    CodaMemProduct prod(coda_format_grib);
    ASSERT_TRUE(prod.ok());
    Grib2MsgSpec m;
    Grib2DataSpec d;
    d.constituent_type = 0;
    m.data.push_back(d);
    std::vector<CodaMemNode*> msgs = {BuildGrib2UnionMsg(
        prod, m, {MutScope::kData, MutMode::kBadType, "constituentType"})};
    ASSERT_TRUE(SetMessagesRoot(prod, msgs));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
}

}  // namespace
}  // namespace stcorp_harp
