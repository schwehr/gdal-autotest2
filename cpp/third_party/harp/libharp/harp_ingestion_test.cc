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
// Unit tests for HARP product ingestion, filtering during ingestion, metadata
// ingestion, and ingestion testing (libharp/harp-ingestion.c).
//
// Google3 build notes: HDF4 is disabled (#undef HAVE_HDF4); udunits2 XML is
// embedded via //third_party/udunits; no checked-in .codadef or testdata files
// are used (minimal stored-ZIP .codadef and NetCDF-3/HDF5/XML inputs are
// synthesized per test inside ::testing::TempDir()).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingestion.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingestion.h

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/stcorp_coda/libcoda/coda.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

int QuietPrint(const char* /*fmt*/, ...) { return 0; }

int CapturePrint(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return 0;
}

void AppendU16Le(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void AppendU32Le(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void AppendU32Be(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendI32Be(std::vector<uint8_t>& out, int32_t v) {
  AppendU32Be(out, static_cast<uint32_t>(v));
}

void AppendF64Be(std::vector<uint8_t>& out, double v) {
  const uint64_t bits = std::bit_cast<uint64_t>(v);
  AppendU32Be(out, static_cast<uint32_t>(bits >> 32));
  AppendU32Be(out, static_cast<uint32_t>(bits & 0xFFFFFFFFULL));
}

void AppendNetcdfString(std::vector<uint8_t>& out, std::string_view s) {
  AppendU32Be(out, static_cast<uint32_t>(s.size()));
  for (char c : s) {
    out.push_back(static_cast<uint8_t>(c));
  }
  while (out.size() % 4 != 0) {
    out.push_back(0);
  }
}

struct ZipEntrySpec {
  std::string_view name;
  std::string_view data;
};

// Writes an uncompressed (method=0) ZIP archive acceptable to libcoda's
// ziparchive.c reader for self-describing .codadef index.xml definitions.
bool WriteStoredZipCodadef(const std::filesystem::path& path,
                           std::span<const ZipEntrySpec> entries) {
  std::vector<uint8_t> buf;
  struct CentralInfo {
    uint32_t offset;
    ZipEntrySpec spec;
  };
  std::vector<CentralInfo> central;
  central.reserve(entries.size());

  for (const auto& entry : entries) {
    const uint32_t offset = static_cast<uint32_t>(buf.size());
    central.push_back({offset, entry});
    const uint16_t name_len = static_cast<uint16_t>(entry.name.size());
    const uint32_t data_len = static_cast<uint32_t>(entry.data.size());

    // Local file header (30 bytes + name + data).
    AppendU32Le(buf, 0x04034B50U);
    AppendU16Le(buf, 20);  // version needed
    AppendU16Le(buf, 0);   // flags
    AppendU16Le(buf, 0);   // compression = stored
    AppendU16Le(buf, 0);   // mod time
    AppendU16Le(buf, 0);   // mod date
    AppendU32Le(buf, 0);   // crc (ziparchive checks header/central equality)
    AppendU32Le(buf, data_len);
    AppendU32Le(buf, data_len);
    AppendU16Le(buf, name_len);
    AppendU16Le(buf, 0);  // extrafield len
    for (char c : entry.name) {
      buf.push_back(static_cast<uint8_t>(c));
    }
    for (char c : entry.data) {
      buf.push_back(static_cast<uint8_t>(c));
    }
  }

  const uint32_t cd_offset = static_cast<uint32_t>(buf.size());
  for (const auto& c : central) {
    const uint16_t name_len = static_cast<uint16_t>(c.spec.name.size());
    const uint32_t data_len = static_cast<uint32_t>(c.spec.data.size());

    // Central directory file header (46 bytes + name).
    AppendU32Le(buf, 0x02014B50U);
    AppendU16Le(buf, 20);  // version made by
    AppendU16Le(buf, 20);  // version needed
    AppendU16Le(buf, 0);   // flags
    AppendU16Le(buf, 0);   // compression = stored
    AppendU16Le(buf, 0);   // mod time
    AppendU16Le(buf, 0);   // mod date
    AppendU32Le(buf, 0);   // crc
    AppendU32Le(buf, data_len);
    AppendU32Le(buf, data_len);
    AppendU16Le(buf, name_len);
    AppendU16Le(buf, 0);  // extrafield len
    AppendU16Le(buf, 0);  // comment len
    AppendU16Le(buf, 0);  // disk number start
    AppendU16Le(buf, 0);  // internal attrs
    AppendU32Le(buf, 0);  // external attrs
    AppendU32Le(buf, c.offset);
    for (char ch : c.spec.name) {
      buf.push_back(static_cast<uint8_t>(ch));
    }
  }

  const uint32_t cd_size = static_cast<uint32_t>(buf.size()) - cd_offset;
  // End of central directory record (22 bytes).
  AppendU32Le(buf, 0x06054B50U);
  AppendU16Le(buf, 0);
  AppendU16Le(buf, 0);
  AppendU16Le(buf, static_cast<uint16_t>(entries.size()));
  AppendU16Le(buf, static_cast<uint16_t>(entries.size()));
  AppendU32Le(buf, cd_size);
  AppendU32Le(buf, cd_offset);
  AppendU16Le(buf, 0);

  return WriteFileBytes(path, buf);
}

// Writes a minimal NetCDF-3 classic file matching the TEMIS_o3field schema
// (or missing O3_column when `include_o3_column` is false).
bool WriteTemisNetcdf3(const std::filesystem::path& path,
                       bool include_o3_column = true) {
  constexpr uint32_t kNcDimension = 0x0000000AU;
  constexpr uint32_t kNcVariable = 0x0000000BU;
  constexpr uint32_t kNcAttribute = 0x0000000CU;
  constexpr uint32_t kNcInt = 4;
  constexpr uint32_t kNcDouble = 6;

  std::vector<uint8_t> buf;
  // Magic + numrecs = 0.
  buf.insert(buf.end(), {'C', 'D', 'F', 1});
  AppendU32Be(buf, 0);

  // Dimensions: lat=2, lon=2.
  AppendU32Be(buf, kNcDimension);
  AppendU32Be(buf, 2);
  AppendNetcdfString(buf, "lat");
  AppendU32Be(buf, 2);
  AppendNetcdfString(buf, "lon");
  AppendU32Be(buf, 2);

  // Global attributes (5).
  AppendU32Be(buf, kNcAttribute);
  AppendU32Be(buf, 5);

  AppendNetcdfString(buf, "Ozone_field_date");
  AppendU32Be(buf, kNcInt);
  AppendU32Be(buf, 2);
  AppendI32Be(buf, 2020);
  AppendI32Be(buf, 5);

  AppendNetcdfString(buf, "Number_of_longitudes");
  AppendU32Be(buf, kNcInt);
  AppendU32Be(buf, 1);
  AppendI32Be(buf, 2);

  AppendNetcdfString(buf, "Longitude_range");
  AppendU32Be(buf, kNcDouble);
  AppendU32Be(buf, 2);
  AppendF64Be(buf, -180.0);
  AppendF64Be(buf, 180.0);

  AppendNetcdfString(buf, "Number_of_latitudes");
  AppendU32Be(buf, kNcInt);
  AppendU32Be(buf, 1);
  AppendI32Be(buf, 2);

  AppendNetcdfString(buf, "Latitude_range");
  AppendU32Be(buf, kNcDouble);
  AppendU32Be(buf, 2);
  AppendF64Be(buf, -90.0);
  AppendF64Be(buf, 90.0);

  // Variables: O3_column(lat, lon) [optional] and O3_std(lat, lon).
  const uint32_t num_vars = include_o3_column ? 2U : 1U;
  AppendU32Be(buf, kNcVariable);
  AppendU32Be(buf, num_vars);

  size_t begin_offset_col = 0;
  if (include_o3_column) {
    AppendNetcdfString(buf, "O3_column");
    AppendU32Be(buf, 2);
    AppendU32Be(buf, 0);
    AppendU32Be(buf, 1);
    AppendU32Be(buf, 0);  // vatt_list ABSENT
    AppendU32Be(buf, 0);
    AppendU32Be(buf, kNcDouble);
    AppendU32Be(buf, 32);
    begin_offset_col = buf.size();
    AppendU32Be(buf, 0);  // placeholder
  }

  AppendNetcdfString(buf, "O3_std");
  AppendU32Be(buf, 2);
  AppendU32Be(buf, 0);
  AppendU32Be(buf, 1);
  AppendU32Be(buf, 0);  // vatt_list ABSENT
  AppendU32Be(buf, 0);
  AppendU32Be(buf, kNcDouble);
  AppendU32Be(buf, 32);
  const size_t begin_offset_std = buf.size();
  AppendU32Be(buf, 0);  // placeholder

  // Patch variable begin offsets now that the header size is fixed.
  const auto patch_u32be = [&](size_t pos, uint32_t val) {
    buf[pos] = static_cast<uint8_t>((val >> 24) & 0xFF);
    buf[pos + 1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    buf[pos + 2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    buf[pos + 3] = static_cast<uint8_t>(val & 0xFF);
  };
  uint32_t data_pos = static_cast<uint32_t>(buf.size());
  if (include_o3_column) {
    patch_u32be(begin_offset_col, data_pos);
    for (double v : {300.0, 310.0, 320.0, 330.0}) {
      AppendF64Be(buf, v);
    }
    data_pos += 32;
  }
  patch_u32be(begin_offset_std, data_pos);
  for (double v : {5.0, 6.0, 7.0, 8.0}) {
    AppendF64Be(buf, v);
  }

  return WriteFileBytes(path, buf);
}

// Writes a minimal stored-ZIP .codadef matching `TEMIS_o3field` on NetCDF.
bool WriteTemisCodadef(const std::filesystem::path& path) {
  constexpr std::string_view kIndexXml =
      R"(<?xml version="1.0" encoding="UTF-8"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="TEMIS">
  <cd:ProductType name="o3field">
    <cd:ProductDefinition id="TEMIS_o3field" format="netcdf" version="1">
      <cd:DetectionRule>
        <cd:MatchExpression>exists(/@Ozone_field_date)</cd:MatchExpression>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)";
  constexpr std::string_view kProductXml =
      R"(<?xml version="1.0" encoding="UTF-8"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="TEMIS_o3field" format="netcdf"/>
)";
  const ZipEntrySpec entries[] = {
      {.name = "VERSION", .data = "20260101"},
      {.name = "index.xml", .data = kIndexXml},
      {.name = "products/TEMIS_o3field.xml", .data = kProductXml},
  };
  return WriteStoredZipCodadef(path, entries);
}

// Configurable synthetic ingestion module state used to exercise every generic
// ingestion, buffering, filter-pushdown, metadata, and error path in
// libharp/harp-ingestion.c without external .codadef or binary testdata.
struct SyntheticConfig {
  bool fail_init = false;
  bool fail_read_dimensions = false;
  bool fail_read_datetime_range = false;
  bool fail_read_scalar = false;
  bool fail_read_1d = false;
  bool fail_read_2d = false;
  bool fail_read_vertical_1d = false;
  bool fail_read_range = false;
  bool fail_read_lon_block = false;
  bool fail_read_label_2d = false;
  bool fail_read_datetime = false;
  bool fail_read_lat_bounds = false;
  bool fail_read_lon_bounds = false;
  bool empty_time_dim = false;
  bool empty_vertical_dim = false;
  bool exclude_all_vars = false;
  bool include_lat_lon = true;
  bool include_lon = true;
  bool include_bounds = true;
  bool include_lon_bounds = true;
  bool include_vertical_vars = true;
  bool include_index = true;
  bool include_datetime = true;
  bool flip_include_lat = false;
  int include_lat_calls = 0;
  bool register_datetime_range = true;
  bool lat_lon_convertible_unit = true;
  bool scalar_string_match = true;
  int product_variant = 0;  // 0=normal, 1=scalar/1d-bounds, 2=invalid,
                            // 3=mismatch, 4=short, 5..13=error variants
  long num_times = 5;
  long num_vertical = 3;
  long optimal_range_len = 2;
};

SyntheticConfig* g_synth_cfg = nullptr;

struct SyntheticUserData {
  int dummy = 0;
};

int SynthReadDimensions(void* /*user_data*/,
                        long dimension[HARP_NUM_DIM_TYPES]) {
  if (g_synth_cfg->fail_read_dimensions) {
    harp_set_error(HARP_ERROR_INGESTION, "synthetic read_dimensions failure");
    return -1;
  }
  dimension[harp_dimension_time] =
      g_synth_cfg->empty_time_dim ? 0 : g_synth_cfg->num_times;
  dimension[harp_dimension_vertical] =
      g_synth_cfg->empty_vertical_dim ? 0 : g_synth_cfg->num_vertical;
  dimension[harp_dimension_latitude] = 2;
  return 0;
}

int SynthReadDatetimeRange(void* /*user_data*/, double* start, double* stop) {
  if (g_synth_cfg->fail_read_datetime_range) {
    harp_set_error(HARP_ERROR_INGESTION,
                   "synthetic read_datetime_range failure");
    return -1;
  }
  *start = 100.0;
  *stop = 104.0;
  return 0;
}

int SynthIncludeAlways(void* /*user_data*/) {
  return g_synth_cfg->exclude_all_vars ? 0 : 1;
}

int SynthIncludeNever(void* /*user_data*/) { return 0; }

int SynthIncludeLat(void* /*user_data*/) {
  if (g_synth_cfg->exclude_all_vars || !g_synth_cfg->include_lat_lon) {
    return 0;
  }
  if (g_synth_cfg->flip_include_lat) {
    return (++g_synth_cfg->include_lat_calls == 1) ? 1 : 0;
  }
  return 1;
}

int SynthIncludeLon(void* /*user_data*/) {
  return (!g_synth_cfg->exclude_all_vars && g_synth_cfg->include_lat_lon &&
          g_synth_cfg->include_lon)
             ? 1
             : 0;
}

int SynthIncludeBounds(void* /*user_data*/) {
  return (!g_synth_cfg->exclude_all_vars && g_synth_cfg->include_bounds) ? 1
                                                                         : 0;
}

int SynthIncludeLonBounds(void* /*user_data*/) {
  return (!g_synth_cfg->exclude_all_vars && g_synth_cfg->include_bounds &&
          g_synth_cfg->include_lon_bounds)
             ? 1
             : 0;
}

int SynthIncludeIndex(void* /*user_data*/) {
  return (!g_synth_cfg->exclude_all_vars && g_synth_cfg->include_index) ? 1 : 0;
}

int SynthIncludeDatetime(void* /*user_data*/) {
  return (!g_synth_cfg->exclude_all_vars && g_synth_cfg->include_datetime) ? 1
                                                                           : 0;
}

int SynthIncludeVertical(void* /*user_data*/) {
  return (!g_synth_cfg->exclude_all_vars && g_synth_cfg->include_vertical_vars)
             ? 1
             : 0;
}

int SynthReadScalarNum(void* /*user_data*/, harp_array data) {
  if (g_synth_cfg->fail_read_scalar) {
    harp_set_error(HARP_ERROR_INGESTION, "synthetic scalar read failure");
    return -1;
  }
  data.double_data[0] = 42.0;
  return 0;
}

int SynthReadScalarStr(void* /*user_data*/, harp_array data) {
  data.string_data[0] =
      strdup(g_synth_cfg->scalar_string_match ? "alpha" : "beta");
  return data.string_data[0] != nullptr ? 0 : -1;
}

int SynthReadDatetime(void* /*user_data*/, harp_array data) {
  if (g_synth_cfg->fail_read_datetime) {
    harp_set_error(HARP_ERROR_INGESTION, "synthetic datetime read failure");
    return -1;
  }
  for (long i = 0; i < g_synth_cfg->num_times; ++i) {
    data.double_data[i] = 100.0 + static_cast<double>(i);
  }
  return 0;
}

int SynthReadLatAll(void* /*user_data*/, harp_array data) {
  if (g_synth_cfg->fail_read_1d) {
    harp_set_error(HARP_ERROR_INGESTION, "synthetic 1d read failure");
    return -1;
  }
  // 5 samples: [0.1, 0.2, 5.0, 25.0, 26.0] deg.
  constexpr double kLats[] = {0.1, 0.2, 5.0, 25.0, 26.0};
  for (long i = 0; i < g_synth_cfg->num_times; ++i) {
    data.double_data[i] = kLats[i % 5];
  }
  return 0;
}

int SynthReadLonBlock(void* /*user_data*/, long index, harp_array data) {
  if (g_synth_cfg->fail_read_lon_block) {
    harp_set_error(HARP_ERROR_INGESTION, "synthetic lon block read failure");
    return -1;
  }
  constexpr double kLons[] = {0.1, 0.2, 5.0, 25.0, 26.0};
  data.double_data[0] = kLons[index % 5];
  return 0;
}

int SynthReadScalarLat(void* /*user_data*/, harp_array data) {
  data.double_data[0] = 5.0;
  return 0;
}

int SynthReadInvalidLat(void* /*user_data*/, harp_array data) {
  data.double_data[0] = 1.0;
  data.double_data[1] = 2.0;
  return 0;
}

int SynthReadStatusEnumAll(void* /*user_data*/, harp_array data) {
  // Small int8 array so subsequent larger double read_all triggers
  // read_buffer_resize().
  for (long i = 0; i < g_synth_cfg->num_times; ++i) {
    data.int8_data[i] = static_cast<int8_t>(i % 2);
  }
  return 0;
}

long SynthGetOptimalRangeLen(void* /*user_data*/) {
  return g_synth_cfg->optimal_range_len;
}

int SynthReadPressureRange(void* /*user_data*/, long index_offset,
                           long index_length, harp_array data) {
  if (g_synth_cfg->fail_read_range) {
    harp_set_error(HARP_ERROR_INGESTION, "synthetic read_range failure");
    return -1;
  }
  for (long i = 0; i < index_length; ++i) {
    data.double_data[i] = 1000.0 - 10.0 * static_cast<double>(index_offset + i);
  }
  return 0;
}

int SynthReadTag1DBlock(void* /*user_data*/, long index, harp_array data) {
  constexpr const char* kTags[] = {"keep_a", "keep_b", "drop_c", "keep_d",
                                   "drop_e"};
  data.string_data[0] = strdup(kTags[index % 5]);
  return data.string_data[0] != nullptr ? 0 : -1;
}

int SynthReadAlt2DRange(void* /*user_data*/, long index_offset,
                        long index_length, harp_array data) {
  if (g_synth_cfg->fail_read_2d) {
    harp_set_error(HARP_ERROR_INGESTION, "synthetic 2d read failure");
    return -1;
  }
  const long nv = g_synth_cfg->num_vertical;
  for (long t = 0; t < index_length; ++t) {
    for (long v = 0; v < nv; ++v) {
      // Sample t=2 gets negative values so all vertical levels fail > 0.
      const double val =
          ((index_offset + t) == 2)
              ? -1.0
              : static_cast<double>((index_offset + t) * 10 + v + 1);
      data.double_data[t * nv + v] = val;
    }
  }
  return 0;
}

int SynthReadLabel2DBlock(void* /*user_data*/, long index, harp_array data) {
  if (g_synth_cfg->fail_read_label_2d) {
    harp_set_error(HARP_ERROR_INGESTION, "synthetic 2d label read failure");
    return -1;
  }
  const long nv = g_synth_cfg->num_vertical;
  for (long v = 0; v < nv; ++v) {
    data.string_data[v] =
        strdup((v == 1) ? "bad" : ((index == 0) ? "ok0" : "okN"));
    if (data.string_data[v] == nullptr) {
      return -1;
    }
  }
  return 0;
}

int SynthReadTimeIndepAll(void* /*user_data*/, harp_array data) {
  for (long i = 0; i < g_synth_cfg->num_times * 2; ++i) {
    data.double_data[i] = static_cast<double>(i);
  }
  return 0;
}

int SynthReadTimeIndepRange(void* /*user_data*/, long index_offset,
                            long index_length, harp_array data) {
  for (long i = 0; i < index_length * 2; ++i) {
    data.double_data[i] = static_cast<double>(index_offset * 2 + i);
  }
  return 0;
}

int SynthReadTimeLatVertBlock(void* /*user_data*/, long index,
                              harp_array data) {
  const long nv = g_synth_cfg->num_vertical;
  for (long k = 0; k < 2 * nv; ++k) {
    data.double_data[k] = static_cast<double>(index * 100 + k);
  }
  return 0;
}

int SynthReadVertical1D(void* /*user_data*/, harp_array data) {
  if (g_synth_cfg->fail_read_vertical_1d) {
    harp_set_error(HARP_ERROR_INGESTION, "synthetic vertical 1d read failure");
    return -1;
  }
  for (long v = 0; v < g_synth_cfg->num_vertical; ++v) {
    data.double_data[v] = static_cast<double>(v + 1) * 100.0;
  }
  return 0;
}

int SynthReadLatVertical2D(void* /*user_data*/, harp_array data) {
  const long nv = g_synth_cfg->num_vertical;
  for (long lat = 0; lat < 2; ++lat) {
    for (long v = 0; v < nv; ++v) {
      data.double_data[lat * nv + v] = static_cast<double>(lat * 100 + v);
    }
  }
  return 0;
}

int SynthReadLatTime2D(void* /*user_data*/, harp_array data) {
  const long nt = g_synth_cfg->num_times;
  for (long lat = 0; lat < 2; ++lat) {
    for (long t = 0; t < nt; ++t) {
      data.double_data[lat * nt + t] = static_cast<double>(lat * 100 + t);
    }
  }
  return 0;
}

int SynthReadIndep1D(void* /*user_data*/, harp_array data) {
  data.double_data[0] = 1.0;
  data.double_data[1] = 2.0;
  return 0;
}

int SynthReadSingleTimeBlock(void* /*user_data*/, long /*index*/,
                             harp_array data) {
  data.double_data[0] = 7.0;
  return 0;
}

int SynthReadSingleTimeAll(void* /*user_data*/, harp_array data) {
  data.double_data[0] = 9.0;
  return 0;
}

int SynthReadLatBounds2DBlock(void* /*user_data*/, long index,
                              harp_array data) {
  if (g_synth_cfg->fail_read_lat_bounds) {
    harp_set_error(HARP_ERROR_INGESTION, "synthetic lat bounds read failure");
    return -1;
  }
  // Boxes: t=0,1 inside [0,10]x[0,10]; t=2,3,4 around [20,30]x[20,30].
  const double base = (index < 2) ? 0.0 : 20.0;
  data.double_data[0] = base + 1.0;
  data.double_data[1] = base + 1.0;
  data.double_data[2] = base + 9.0;
  data.double_data[3] = base + 9.0;
  return 0;
}

int SynthReadLonBounds2DBlock(void* /*user_data*/, long index,
                              harp_array data) {
  if (g_synth_cfg->fail_read_lon_bounds) {
    harp_set_error(HARP_ERROR_INGESTION, "synthetic lon bounds read failure");
    return -1;
  }
  const double base = (index < 2) ? 0.0 : 20.0;
  data.double_data[0] = base + 1.0;
  data.double_data[1] = base + 9.0;
  data.double_data[2] = base + 9.0;
  data.double_data[3] = base + 1.0;
  return 0;
}

int SynthReadZeroBounds2DBlock(void* /*user_data*/, long /*index*/,
                               harp_array data) {
  data.double_data[0] = 0.0;
  data.double_data[1] = 0.0;
  data.double_data[2] = 0.0;
  data.double_data[3] = 0.0;
  return 0;
}

int SynthReadLatBounds1DAll(void* /*user_data*/, harp_array data) {
  data.double_data[0] = 1.0;
  data.double_data[1] = 1.0;
  data.double_data[2] = 9.0;
  data.double_data[3] = 9.0;
  return 0;
}

int SynthReadLonBounds1DAll(void* /*user_data*/, harp_array data) {
  data.double_data[0] = 1.0;
  data.double_data[1] = 9.0;
  data.double_data[2] = 9.0;
  data.double_data[3] = 1.0;
  return 0;
}

int SynthReadBounds1ElementAll(void* /*user_data*/, harp_array data) {
  data.double_data[0] = 1.0;
  return 0;
}

int SynthReadBounds3DAll(void* /*user_data*/, harp_array data) {
  for (long i = 0; i < g_synth_cfg->num_times * 4; ++i) {
    data.double_data[i] = 1.0;
  }
  return 0;
}

int SynthReadIndex(void* /*user_data*/, long index, harp_array data) {
  data.int32_data[0] = static_cast<int32_t>(index);
  return 0;
}

int SynthIngestionInit(const harp_ingestion_module* module,
                       coda_product* /*product*/,
                       const harp_ingestion_options* options,
                       harp_product_definition** definition, void** user_data) {
  if (g_synth_cfg->fail_init) {
    harp_set_error(HARP_ERROR_INGESTION, "synthetic init failure");
    return -1;
  }
  const char* val = nullptr;
  if (harp_ingestion_options_has_option(options, "mode") &&
      harp_ingestion_options_get_option(options, "mode", &val) == 0 &&
      std::string_view(val) == "bad") {
    harp_set_error(HARP_ERROR_INGESTION, "synthetic bad option value");
    return -1;
  }
  auto* info = static_cast<SyntheticUserData*>(
      std::calloc(1, sizeof(SyntheticUserData)));
  if (info == nullptr) {
    return -1;
  }
  *definition = module->product_definition[g_synth_cfg->product_variant];
  *user_data = info;
  return 0;
}

void SynthIngestionDone(void* user_data) { std::free(user_data); }

// Writes the XML .codadef and XML product file before harp_ingestion_init()
// initializes libcoda's data dictionary.
void WriteSyntheticCodadefAndXml(const TempDir& dir, const TempFile& xml_file) {
  constexpr std::string_view kIndexXml =
      R"(<?xml version="1.0" encoding="UTF-8"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="SYNTH_TEST">
  <cd:ProductType name="prod">
    <cd:ProductDefinition id="SYNTH_TEST_prod" format="xml" version="1">
      <cd:DetectionRule>
        <cd:MatchExpression>exists(/root/synthetic_marker)</cd:MatchExpression>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)";
  constexpr std::string_view kProductXml =
      R"(<?xml version="1.0" encoding="UTF-8"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="SYNTH_TEST_prod" format="xml"/>
)";
  const ZipEntrySpec entries[] = {
      {.name = "VERSION", .data = "20260101"},
      {.name = "index.xml", .data = kIndexXml},
      {.name = "products/SYNTH_TEST_prod.xml", .data = kProductXml},
  };
  ASSERT_TRUE(WriteStoredZipCodadef(dir.child("synth.codadef"), entries));
  ASSERT_TRUE(
      WriteFileText(xml_file.path(),
                    "<?xml version=\"1.0\"?>\n"
                    "<root><synthetic_marker>1</synthetic_marker></root>\n"));
}

// Registers the synthetic ingestion module with HARP (must be called after
// harp_ingestion_init()).
void RegisterSyntheticModule(bool with_options) {
  harp_ingestion_module* mod = harp_ingestion_register_module(
      "SYNTH_TEST_prod", "SYNTH_TEST", "SYNTH_TEST", "prod",
      "Synthetic test module", SynthIngestionInit, SynthIngestionDone);
  ASSERT_NE(nullptr, mod);

  if (with_options) {
    const char* mode_vals[] = {"good", "bad"};
    const char* extra_vals[] = {"one", "two"};
    harp_ingestion_register_option(mod, "mode", "mode option", 2, mode_vals);
    harp_ingestion_register_option(mod, "extra", "extra option", 2, extra_vals);
  }

  harp_product_definition* pdef = harp_ingestion_register_product(
      mod, "SYNTH_TEST_prod", "Synthetic product", SynthReadDimensions);
  ASSERT_NE(nullptr, pdef);

  if (g_synth_cfg->register_datetime_range) {
    harp_ingestion_register_datetime_range_read(pdef, SynthReadDatetimeRange);
  }

  const harp_dimension_type kTimeDim[] = {harp_dimension_time};
  const harp_dimension_type kVertDim[] = {harp_dimension_vertical};
  const harp_dimension_type kLatDim[] = {harp_dimension_latitude};
  const harp_dimension_type kTimeVertDim[] = {harp_dimension_time,
                                              harp_dimension_vertical};
  const harp_dimension_type kLatVertDim[] = {harp_dimension_latitude,
                                             harp_dimension_vertical};
  const harp_dimension_type kLatTimeDim[] = {harp_dimension_latitude,
                                             harp_dimension_time};
  const harp_dimension_type kTimeLatVertDim[] = {
      harp_dimension_time, harp_dimension_latitude, harp_dimension_vertical};
  const harp_dimension_type kTimeIndep2Dim[] = {harp_dimension_time,
                                                harp_dimension_independent};
  const long kTimeIndep2Len[] = {-1, 2};
  const harp_dimension_type kIndep2Dim[] = {harp_dimension_independent};
  const long kIndep2Len[] = {2};
  const harp_dimension_type kBounds2DDim[] = {harp_dimension_time,
                                              harp_dimension_independent};
  const long kBounds2DLen[] = {-1, 4};
  const long kBounds1ElemLen[] = {-1, 1};
  const harp_dimension_type kBounds1DDim[] = {harp_dimension_independent};
  const long kBounds1DLen[] = {4};
  const harp_dimension_type kBounds3DDim[] = {harp_dimension_time,
                                              harp_dimension_independent,
                                              harp_dimension_independent};
  const long kBounds3DLen[] = {-1, 2, 2};
  const long kSingleIndepLen[] = {1};

  harp_ingestion_register_variable_block_read(
      pdef, "index", harp_type_int32, 1, kTimeDim, nullptr, "sample index",
      nullptr, SynthIncludeIndex, SynthReadIndex);

  harp_ingestion_register_variable_full_read(
      pdef, "scalar_num", harp_type_double, 0, nullptr, nullptr, nullptr,
      nullptr, SynthIncludeAlways, SynthReadScalarNum);

  harp_ingestion_register_variable_full_read(
      pdef, "scalar_str", harp_type_string, 0, nullptr, nullptr, "scalar str",
      nullptr, SynthIncludeAlways, SynthReadScalarStr);

  harp_ingestion_register_variable_full_read(
      pdef, "datetime", harp_type_double, 1, kTimeDim, nullptr, "datetime",
      "days since 2000-01-01", SynthIncludeDatetime, SynthReadDatetime);

  harp_ingestion_register_variable_full_read(
      pdef, "latitude", harp_type_double, 1, kTimeDim, nullptr, "latitude",
      g_synth_cfg->lat_lon_convertible_unit ? "degree_north" : "m",
      SynthIncludeLat, SynthReadLatAll);

  harp_ingestion_register_variable_block_read(
      pdef, "longitude", harp_type_double, 1, kTimeDim, nullptr, "longitude",
      g_synth_cfg->lat_lon_convertible_unit ? "degree_east" : "m",
      SynthIncludeLon, SynthReadLonBlock);

  // Enum + int8 read_all variable (tested before larger double read_all).
  harp_variable_definition* enum_var =
      harp_ingestion_register_variable_full_read(
          pdef, "status_flag", harp_type_int8, 1, kTimeDim, nullptr,
          "status flag", nullptr, SynthIncludeAlways, SynthReadStatusEnumAll);
  const char* enum_names[] = {"ok", "warn"};
  harp_variable_definition_set_enumeration_values(enum_var, 2, enum_names);

  // read_range variable with optimal_range_len = 2 (smaller than time = 5).
  harp_ingestion_register_variable_range_read(
      pdef, "pressure", harp_type_double, 1, kTimeDim, nullptr, "pressure",
      "hPa", SynthIncludeAlways, SynthGetOptimalRangeLen,
      SynthReadPressureRange);

  // 1D string variable using read_block.
  harp_ingestion_register_variable_block_read(
      pdef, "tag", harp_type_string, 1, kTimeDim, nullptr, "string tag",
      nullptr, SynthIncludeAlways, SynthReadTag1DBlock);

  // 2D [time, vertical] range-read numeric variable.
  harp_ingestion_register_variable_range_read(
      pdef, "altitude", harp_type_double, 2, kTimeVertDim, nullptr, "altitude",
      "m", SynthIncludeVertical, SynthGetOptimalRangeLen, SynthReadAlt2DRange);

  // 2D [time, vertical] block-read string variable (covers
  // read_buffer_free_string_data inside secondary mask loop).
  harp_ingestion_register_variable_block_read(
      pdef, "level_label", harp_type_string, 2, kTimeVertDim, nullptr,
      "level label", nullptr, SynthIncludeVertical, SynthReadLabel2DBlock);

  // Non-time-dependent 1D [vertical] and 2D [latitude, vertical] variables
  // (exercises has_2D_masks && dimension_type[0] != harp_dimension_time).
  harp_ingestion_register_variable_full_read(
      pdef, "vertical_grid", harp_type_double, 1, kVertDim, nullptr,
      "vertical grid", "m", SynthIncludeVertical, SynthReadVertical1D);

  harp_ingestion_register_variable_full_read(
      pdef, "lat_vertical_field", harp_type_double, 2, kLatVertDim, nullptr,
      "lat-vert field", "1", SynthIncludeVertical, SynthReadLatVertical2D);

  // 2D [time, independent] read_all/read_range and 3D [time, latitude,
  // vertical] variables covering independent dim handling in read_block() and
  // unmasked secondary dims in get_variable().
  harp_ingestion_register_variable_full_read(
      pdef, "time_indep_all", harp_type_double, 2, kTimeIndep2Dim,
      kTimeIndep2Len, "time indep all", nullptr, SynthIncludeAlways,
      SynthReadTimeIndepAll);
  harp_ingestion_register_variable_range_read(
      pdef, "time_indep_range", harp_type_double, 2, kTimeIndep2Dim,
      kTimeIndep2Len, "time indep range", nullptr, SynthIncludeAlways,
      SynthGetOptimalRangeLen, SynthReadTimeIndepRange);
  harp_ingestion_register_variable_block_read(
      pdef, "time_lat_vert_var", harp_type_double, 3, kTimeLatVertDim, nullptr,
      "time lat vert", nullptr, SynthIncludeVertical,
      SynthReadTimeLatVertBlock);

  // 1D independent variable (triggers invalid dimensions for filtering).
  harp_ingestion_register_variable_full_read(
      pdef, "indep_axis", harp_type_double, 1, kIndep2Dim, kIndep2Len,
      "independent axis", nullptr, SynthIncludeAlways, SynthReadIndep1D);

  // Single-element [independent(1)] variables exercising dim[0] == 1 fast
  // paths in read_all() and read_block().
  harp_ingestion_register_variable_block_read(
      pdef, "single_block_var", harp_type_double, 1, kIndep2Dim,
      kSingleIndepLen, "single block", nullptr, SynthIncludeAlways,
      SynthReadSingleTimeBlock);
  harp_ingestion_register_variable_full_read(
      pdef, "single_all_var", harp_type_double, 1, kIndep2Dim, kSingleIndepLen,
      "single all", nullptr, SynthIncludeAlways, SynthReadSingleTimeAll);

  // Unavailable variable (include == 0).
  harp_ingestion_register_variable_full_read(
      pdef, "excluded_var", harp_type_double, 0, nullptr, nullptr, "excluded",
      nullptr, SynthIncludeNever, SynthReadScalarNum);

  harp_ingestion_register_variable_block_read(
      pdef, "latitude_bounds", harp_type_double, 2, kBounds2DDim, kBounds2DLen,
      "lat bounds", "degree_north", SynthIncludeBounds,
      SynthReadLatBounds2DBlock);
  harp_ingestion_register_variable_block_read(
      pdef, "longitude_bounds", harp_type_double, 2, kBounds2DDim, kBounds2DLen,
      "lon bounds", "degree_east", SynthIncludeLonBounds,
      SynthReadLonBounds2DBlock);

  // Variant 1: scalar lat/lon and 1D bounds (expanded to time dim in filters).
  harp_product_definition* pdef_scalar = harp_ingestion_register_product(
      mod, "SYNTH_TEST_scalar", "Synthetic scalar lat/lon",
      SynthReadDimensions);
  ASSERT_NE(nullptr, pdef_scalar);
  harp_ingestion_register_variable_full_read(
      pdef_scalar, "datetime", harp_type_double, 1, kTimeDim, nullptr,
      "datetime", "days since 2000-01-01", SynthIncludeAlways,
      SynthReadDatetime);
  harp_ingestion_register_variable_full_read(
      pdef_scalar, "latitude", harp_type_double, 0, nullptr, nullptr,
      "scalar lat", "degree_north", SynthIncludeAlways, SynthReadScalarLat);
  harp_ingestion_register_variable_full_read(
      pdef_scalar, "longitude", harp_type_double, 0, nullptr, nullptr,
      "scalar lon", "degree_east", SynthIncludeAlways, SynthReadScalarLat);
  harp_ingestion_register_variable_full_read(
      pdef_scalar, "latitude_bounds", harp_type_double, 1, kBounds1DDim,
      kBounds1DLen, "lat bounds 1d", "degree_north", SynthIncludeAlways,
      SynthReadLatBounds1DAll);
  harp_ingestion_register_variable_full_read(
      pdef_scalar, "longitude_bounds", harp_type_double, 1, kBounds1DDim,
      kBounds1DLen, "lon bounds 1d", "degree_east", SynthIncludeAlways,
      SynthReadLonBounds1DAll);

  // Variant 2: invalid lat/lon dimensions and invalid 3D bounds dimensions.
  harp_product_definition* pdef_invalid = harp_ingestion_register_product(
      mod, "SYNTH_TEST_invalid", "Synthetic invalid dims", SynthReadDimensions);
  ASSERT_NE(nullptr, pdef_invalid);
  harp_ingestion_register_variable_full_read(
      pdef_invalid, "latitude", harp_type_double, 1, kLatDim, nullptr,
      "bad lat", "degree_north", SynthIncludeAlways, SynthReadInvalidLat);
  harp_ingestion_register_variable_full_read(
      pdef_invalid, "longitude", harp_type_double, 1, kLatDim, nullptr,
      "bad lon", "degree_east", SynthIncludeAlways, SynthReadInvalidLat);
  harp_ingestion_register_variable_full_read(
      pdef_invalid, "latitude_bounds", harp_type_double, 3, kBounds3DDim,
      kBounds3DLen, "lat bounds 3d", "degree_north", SynthIncludeAlways,
      SynthReadBounds3DAll);
  harp_ingestion_register_variable_full_read(
      pdef_invalid, "longitude_bounds", harp_type_double, 3, kBounds3DDim,
      kBounds3DLen, "lon bounds 3d", "degree_east", SynthIncludeAlways,
      SynthReadBounds3DAll);

  // Variant 3: mismatched bounds independent dimension lengths (4 vs 1).
  harp_product_definition* pdef_mismatch = harp_ingestion_register_product(
      mod, "SYNTH_TEST_mismatch", "Synthetic mismatch bounds",
      SynthReadDimensions);
  ASSERT_NE(nullptr, pdef_mismatch);
  harp_ingestion_register_variable_block_read(
      pdef_mismatch, "latitude_bounds", harp_type_double, 2, kBounds2DDim,
      kBounds2DLen, "lat bounds mismatch", "degree_north", SynthIncludeAlways,
      SynthReadLatBounds2DBlock);
  harp_ingestion_register_variable_full_read(
      pdef_mismatch, "longitude_bounds", harp_type_double, 2, kBounds2DDim,
      kBounds1ElemLen, "lon bounds mismatch", "degree_east", SynthIncludeAlways,
      SynthReadBounds1ElementAll);

  // Variant 4: bounds independent dimension length < 2.
  harp_product_definition* pdef_short = harp_ingestion_register_product(
      mod, "SYNTH_TEST_short", "Synthetic short bounds", SynthReadDimensions);
  ASSERT_NE(nullptr, pdef_short);
  harp_ingestion_register_variable_full_read(
      pdef_short, "latitude_bounds", harp_type_double, 2, kBounds2DDim,
      kBounds1ElemLen, "lat bounds short", "degree_north", SynthIncludeAlways,
      SynthReadBounds1ElementAll);
  harp_ingestion_register_variable_full_read(
      pdef_short, "longitude_bounds", harp_type_double, 2, kBounds2DDim,
      kBounds1ElemLen, "lon bounds short", "degree_east", SynthIncludeAlways,
      SynthReadBounds1ElementAll);

  // Variant 5: unconvertible lat/lon units (meters instead of degrees).
  harp_product_definition* pdef_bad_unit = harp_ingestion_register_product(
      mod, "SYNTH_TEST_bad_unit", "Synthetic bad unit", SynthReadDimensions);
  ASSERT_NE(nullptr, pdef_bad_unit);
  harp_ingestion_register_variable_full_read(
      pdef_bad_unit, "latitude", harp_type_double, 1, kTimeDim, nullptr,
      "bad unit lat", "m", SynthIncludeAlways, SynthReadLatAll);
  harp_ingestion_register_variable_block_read(
      pdef_bad_unit, "longitude", harp_type_double, 1, kTimeDim, nullptr,
      "bad unit lon", "m", SynthIncludeAlways, SynthReadLonBlock);

  // Variant 6: valid latitude unit, unconvertible longitude unit.
  harp_product_definition* pdef_bad_lon_unit = harp_ingestion_register_product(
      mod, "SYNTH_TEST_bad_lon_unit", "Bad lon unit", SynthReadDimensions);
  ASSERT_NE(nullptr, pdef_bad_lon_unit);
  harp_ingestion_register_variable_full_read(
      pdef_bad_lon_unit, "latitude", harp_type_double, 1, kTimeDim, nullptr,
      "ok lat", "degree_north", SynthIncludeAlways, SynthReadLatAll);
  harp_ingestion_register_variable_block_read(
      pdef_bad_lon_unit, "longitude", harp_type_double, 1, kTimeDim, nullptr,
      "bad unit lon", "m", SynthIncludeAlways, SynthReadLonBlock);

  // Variant 7: valid latitude dim [time], invalid longitude dim [latitude].
  harp_product_definition* pdef_bad_lon_dim = harp_ingestion_register_product(
      mod, "SYNTH_TEST_bad_lon_dim", "Bad lon dim", SynthReadDimensions);
  ASSERT_NE(nullptr, pdef_bad_lon_dim);
  harp_ingestion_register_variable_full_read(
      pdef_bad_lon_dim, "latitude", harp_type_double, 1, kTimeDim, nullptr,
      "ok lat", "degree_north", SynthIncludeAlways, SynthReadLatAll);
  harp_ingestion_register_variable_full_read(
      pdef_bad_lon_dim, "longitude", harp_type_double, 1, kLatDim, nullptr,
      "bad dim lon", "degree_east", SynthIncludeAlways, SynthReadInvalidLat);

  // Variant 8: unconvertible latitude_bounds unit.
  harp_product_definition* pdef_bad_blat_unit = harp_ingestion_register_product(
      mod, "SYNTH_TEST_bad_blat_unit", "Bad bounds lat unit",
      SynthReadDimensions);
  ASSERT_NE(nullptr, pdef_bad_blat_unit);
  harp_ingestion_register_variable_block_read(
      pdef_bad_blat_unit, "latitude_bounds", harp_type_double, 2, kBounds2DDim,
      kBounds2DLen, "bad lat bounds unit", "m", SynthIncludeAlways,
      SynthReadLatBounds2DBlock);
  harp_ingestion_register_variable_block_read(
      pdef_bad_blat_unit, "longitude_bounds", harp_type_double, 2, kBounds2DDim,
      kBounds2DLen, "ok lon bounds", "degree_east", SynthIncludeAlways,
      SynthReadLonBounds2DBlock);

  // Variant 9: valid latitude_bounds unit, unconvertible longitude_bounds unit.
  harp_product_definition* pdef_bad_blon_unit = harp_ingestion_register_product(
      mod, "SYNTH_TEST_bad_blon_unit", "Bad bounds lon unit",
      SynthReadDimensions);
  ASSERT_NE(nullptr, pdef_bad_blon_unit);
  harp_ingestion_register_variable_block_read(
      pdef_bad_blon_unit, "latitude_bounds", harp_type_double, 2, kBounds2DDim,
      kBounds2DLen, "ok lat bounds", "degree_north", SynthIncludeAlways,
      SynthReadLatBounds2DBlock);
  harp_ingestion_register_variable_block_read(
      pdef_bad_blon_unit, "longitude_bounds", harp_type_double, 2, kBounds2DDim,
      kBounds2DLen, "bad lon bounds unit", "m", SynthIncludeAlways,
      SynthReadLonBounds2DBlock);

  // Variant 10: valid 2D latitude_bounds, invalid 3D longitude_bounds.
  harp_product_definition* pdef_bad_blon_dim = harp_ingestion_register_product(
      mod, "SYNTH_TEST_bad_blon_dim", "Bad bounds lon dim",
      SynthReadDimensions);
  ASSERT_NE(nullptr, pdef_bad_blon_dim);
  harp_ingestion_register_variable_block_read(
      pdef_bad_blon_dim, "latitude_bounds", harp_type_double, 2, kBounds2DDim,
      kBounds2DLen, "ok lat bounds", "degree_north", SynthIncludeAlways,
      SynthReadLatBounds2DBlock);
  harp_ingestion_register_variable_full_read(
      pdef_bad_blon_dim, "longitude_bounds", harp_type_double, 3, kBounds3DDim,
      kBounds3DLen, "bad lon bounds 3d", "degree_east", SynthIncludeAlways,
      SynthReadBounds3DAll);

  // Variant 11: degenerate all-zero polygon bounds failing polygon
  // construction.
  harp_product_definition* pdef_bad_poly = harp_ingestion_register_product(
      mod, "SYNTH_TEST_bad_poly", "Bad polygon geom", SynthReadDimensions);
  ASSERT_NE(nullptr, pdef_bad_poly);
  harp_ingestion_register_variable_block_read(
      pdef_bad_poly, "latitude_bounds", harp_type_double, 2, kBounds2DDim,
      kBounds2DLen, "zero lat bounds", "degree_north", SynthIncludeAlways,
      SynthReadZeroBounds2DBlock);
  harp_ingestion_register_variable_block_read(
      pdef_bad_poly, "longitude_bounds", harp_type_double, 2, kBounds2DDim,
      kBounds2DLen, "zero lon bounds", "degree_east", SynthIncludeAlways,
      SynthReadZeroBounds2DBlock);

  // Variant 12: variable with NaN valid_min that fails harp_product_verify()
  // after ingestion.
  harp_product_definition* pdef_bad_verify = harp_ingestion_register_product(
      mod, "SYNTH_TEST_bad_verify", "Bad verify product", SynthReadDimensions);
  ASSERT_NE(nullptr, pdef_bad_verify);
  harp_variable_definition* bad_verify_var =
      harp_ingestion_register_variable_full_read(
          pdef_bad_verify, "bad_valid_min_var", harp_type_double, 1, kTimeDim,
          nullptr, "NaN valid_min var", "m", SynthIncludeAlways,
          SynthReadLatAll);
  ASSERT_NE(nullptr, bad_verify_var);
  harp_variable_definition_set_valid_range_double(bad_verify_var, harp_nan(),
                                                  10.0);

  // Variant 13: variable with inner time dimension that fails
  // harp_variable_new() inside get_variable().
  harp_product_definition* pdef_bad_lat_time = harp_ingestion_register_product(
      mod, "SYNTH_TEST_bad_lat_time", "Bad lat-time product",
      SynthReadDimensions);
  ASSERT_NE(nullptr, pdef_bad_lat_time);
  harp_ingestion_register_variable_full_read(
      pdef_bad_lat_time, "bad_lat_time", harp_type_double, 2, kLatTimeDim,
      nullptr, "bad inner time dim", "m", SynthIncludeAlways,
      SynthReadLatTime2D);
}

TEST(HarpIngestionTest, NullAndMissingFileErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_product* raw_prod = nullptr;
  EXPECT_EQ(-1, harp_ingest(nullptr, nullptr, nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_ingest("missing.nc", nullptr, nullptr, nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  harp_product_metadata* raw_meta = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
  ScopedHarpProductMetadata meta(raw_meta);
  EXPECT_EQ(-1, harp_ingest_metadata(nullptr, nullptr, meta.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  EXPECT_EQ(-1, harp_ingest_test(nullptr, QuietPrint));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  const TempFile missing("harp_ingestion_missing.nc");
  EXPECT_EQ(-1, harp_ingest(missing.c_str(), nullptr, nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_CODA, harp_errno);
  EXPECT_EQ(-1, harp_ingest_metadata(missing.c_str(), nullptr, meta.get()));
  EXPECT_EQ(-1, harp_ingest_test(missing.c_str(), QuietPrint));

  // Invalid operation syntax in harp_ingest. (Upstream
  // ingestion_options_from_string() in harp-ingestion-options.c:304 leaks
  // `options` on syntax error, failing HeapLeakChecker/LSan if called with a
  // malformed options string.)
  EXPECT_EQ(-1, harp_ingest(missing.c_str(), "invalid===", nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_OPERATION_SYNTAX, harp_errno);

  // Corrupt/non-product bytes fail CODA recognition / unsupported product.
  const TempFile corrupt("harp_ingestion_corrupt.bin");
  ASSERT_TRUE(WriteFileText(corrupt.path(), "not a valid product"));
  EXPECT_EQ(-1, harp_ingest(corrupt.c_str(), nullptr, nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, harp_errno);
  EXPECT_EQ(-1, harp_ingest_test(corrupt.c_str(), CapturePrint));
  EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, harp_errno);
}

TEST(HarpIngestionTest, IngestionInitFailureErrors) {
  const TempFile bad_codadef("harp_ingestion_bad_init/bad.codadef");
  const TempFile dummy_file("harp_ingestion_bad_init/dummy.nc");
  ASSERT_TRUE(WriteFileText(bad_codadef.path(), "not a valid codadef zip"));

  ASSERT_EQ(HARP_SUCCESS, harp_set_coda_definition_path(bad_codadef.c_str()));
  auto reset_coda =
      absl::MakeCleanup([] { harp_set_coda_definition_path(nullptr); });
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_product* raw_prod = nullptr;
  EXPECT_EQ(-1, harp_ingest(dummy_file.c_str(), nullptr, nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_CODA, harp_errno);

  harp_product_metadata* raw_meta = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
  ScopedHarpProductMetadata meta(raw_meta);
  EXPECT_EQ(-1, harp_ingest_metadata(dummy_file.c_str(), nullptr, meta.get()));
  EXPECT_EQ(HARP_ERROR_CODA, harp_errno);

  EXPECT_EQ(-1, harp_ingest_test(dummy_file.c_str(), QuietPrint));
  EXPECT_EQ(HARP_ERROR_CODA, harp_errno);
}

TEST(HarpIngestionTest, EndToEndTemisNetcdfIngestImportAndMetadata) {
  const TempDir dir("harp_ingestion_temis");
  const TempFile nc_file("harp_ingestion_temis/temis_o3field.nc");
  const TempFile bad_nc("harp_ingestion_temis/temis_missing_col.nc");
  const TempFile raw_nc("harp_ingestion_temis/unrecognized.nc");
  ASSERT_TRUE(WriteTemisCodadef(dir.child("temis.codadef")));
  ASSERT_TRUE(WriteTemisNetcdf3(nc_file.path(), /*include_o3_column=*/true));
  ASSERT_TRUE(WriteTemisNetcdf3(bad_nc.path(), /*include_o3_column=*/false));
  // NetCDF-3 header with empty dim/gatt/var lists (32 bytes) is valid NetCDF
  // without Ozone_field_date, so CODA recognizes format=netcdf with no match.
  {
    std::vector<uint8_t> minimal_nc = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0, 0, 0,
                                       0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0,
                                       0,   0,   0,   0, 0, 0, 0, 0, 0, 0};
    ASSERT_TRUE(WriteFileBytes(raw_nc.path(), minimal_nc));
  }

  ASSERT_EQ(HARP_SUCCESS, harp_set_coda_definition_path(dir.c_str()));
  auto reset_coda_path =
      absl::MakeCleanup([] { harp_set_coda_definition_path(nullptr); });
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // 1. Direct harp_ingest() and public harp_import() fallback to harp_ingest().
  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_ingest(nc_file.c_str(), "latitude > 0", nullptr, &raw_prod));
  ScopedHarpProduct ingested(raw_prod);
  EXPECT_EQ(1, ingested->dimension[harp_dimension_latitude]);
  EXPECT_EQ(2, ingested->dimension[harp_dimension_longitude]);

  raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_import(nc_file.c_str(), nullptr, nullptr, &raw_prod));
  ScopedHarpProduct imported(raw_prod);
  harp_variable* o3_var = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(
                imported.get(), "O3_column_number_density", &o3_var));
  EXPECT_EQ(4L, o3_var->num_elements);
  EXPECT_DOUBLE_EQ(300.0, o3_var->data.double_data[0]);

  // 2. Metadata via harp_ingest_metadata() and harp_import_product_metadata().
  harp_product_metadata* raw_meta = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
  ScopedHarpProductMetadata meta(raw_meta);
  ASSERT_EQ(HARP_SUCCESS,
            harp_ingest_metadata(nc_file.c_str(), nullptr, meta.get()));
  EXPECT_STREQ("TEMIS_o3field", meta->format);
  EXPECT_EQ(2, meta->dimension[harp_dimension_latitude]);
  EXPECT_EQ(2, meta->dimension[harp_dimension_longitude]);

  harp_product_metadata* raw_pub_meta = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_import_product_metadata(nc_file.c_str(), nullptr,
                                                       &raw_pub_meta));
  ScopedHarpProductMetadata pub_meta(raw_pub_meta);
  EXPECT_STREQ("TEMIS_o3field", pub_meta->format);

  // 3. harp_ingest_test() and harp_import_test() succeed on valid TEMIS file,
  // return 1 on incomplete TEMIS file, and return -1 on unrecognized NetCDF.
  EXPECT_EQ(0, harp_ingest_test(nc_file.c_str(), CapturePrint));
  EXPECT_EQ(0, harp_import_test(nc_file.c_str(), CapturePrint));
  EXPECT_EQ(1, harp_ingest_test(bad_nc.c_str(), CapturePrint));
  EXPECT_EQ(-1, harp_ingest_test(raw_nc.c_str(), CapturePrint));
  EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, harp_errno);
}

TEST(HarpIngestionTest, EndToEndHdf5AndNetcdfHarpFormatImportAndMetadata) {
  const TempDir dir("harp_ingestion_hdf5");
  const TempFile h5_file("harp_ingestion_hdf5/harp_product.h5");
  const TempFile nc_file("harp_ingestion_hdf5/harp_product.nc");
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);
  const harp_dimension_type kTimeDim[] = {harp_dimension_time};
  const long kTimeLen[] = {2};
  harp_variable* raw_dt = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new("datetime", harp_type_double, 1,
                                            kTimeDim, kTimeLen, &raw_dt));
  ScopedHarpVariable dt_var(raw_dt);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_unit(dt_var.get(), "days since 2000-01-01"));
  dt_var->data.double_data[0] = 10.0;
  dt_var->data.double_data[1] = 12.0;
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), dt_var.get()));
  dt_var.release();

  ASSERT_EQ(HARP_SUCCESS, harp_export(h5_file.c_str(), "hdf5", prod.get()));
  ASSERT_EQ(HARP_SUCCESS, harp_export(nc_file.c_str(), "netcdf", prod.get()));

  for (const auto& [path, expected_fmt] :
       {std::pair{h5_file.c_str(), "HARP_HDF5"},
        std::pair{nc_file.c_str(), "HARP_NETCDF"}}) {
    harp_product* imported = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_import(path, nullptr, nullptr, &imported));
    ScopedHarpProduct imp_guard(imported);
    EXPECT_EQ(2, imp_guard->dimension[harp_dimension_time]);

    harp_product_metadata* meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_import_product_metadata(path, nullptr, &meta));
    ScopedHarpProductMetadata meta_guard(meta);
    EXPECT_STREQ(expected_fmt, meta_guard->format);

    EXPECT_EQ(0, harp_import_test(path, CapturePrint));
  }
}

TEST(HarpIngestionTest, SyntheticModuleReadModesFiltersAndBufferResize) {
  const TempDir dir("harp_ingestion_synth_filters");
  const TempFile xml_file("harp_ingestion_synth_filters/product.xml");
  SyntheticConfig cfg;
  g_synth_cfg = &cfg;
  auto clear_cfg = absl::MakeCleanup([] { g_synth_cfg = nullptr; });

  WriteSyntheticCodadefAndXml(dir, xml_file);
  ASSERT_EQ(HARP_SUCCESS, harp_set_coda_definition_path(dir.c_str()));
  auto reset_coda =
      absl::MakeCleanup([] { harp_set_coda_definition_path(nullptr); });
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_init());
  RegisterSyntheticModule(/*with_options=*/true);

  // 1. Write a collocation CSV matching xml_file basename for
  // operation_collocation_filter.
  const TempFile colloc_file("harp_ingestion_synth_filters/colloc.csv");
  {
    harp_collocation_result* raw_colloc = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_collocation_result_new(&raw_colloc, 0, nullptr, nullptr));
    ScopedHarpCollocationResult colloc(raw_colloc);
    const std::string src_a = xml_file.path().filename().string();
    for (long idx : {0L, 1L, 3L}) {
      ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                                  colloc.get(), idx + 10, src_a.c_str(), idx,
                                  "b.nc", idx, 0, nullptr));
    }
    ASSERT_EQ(HARP_SUCCESS,
              harp_collocation_result_write(colloc_file.c_str(), colloc.get()));
  }

  // 2. Write a polygon area mask CSV for polygon filter tests.
  const TempFile area_file("harp_ingestion_synth_filters/area.csv");
  ASSERT_TRUE(WriteFileText(area_file.path(),
                            "polygon\n"
                            "0.0,0.0, 0.0,10.0, 10.0,10.0, 10.0,0.0\n"));

  // 3. Exercise:
  // - value filters on scalar numeric + scalar string (coalesced)
  // - 1D value filters (int8 status_flag first, then larger double pressure
  //   so read_buffer_resize grows block_buffer)
  // - 2D [time, vertical] value filter (altitude > 0 filters out sample t=2
  //   completely, clearing time_mask[2], and skips already-masked samples)
  // - 1D vertical value filter AFTER 2D mask (reduces 2D mask to 1D & merges)
  // - 2D value filter on vertical AFTER 1D vertical mask (prepend_dimension)
  // - index filters on time and vertical (reducing 2D mask)
  // - coalesced point_distance + point_in_area filters
  // - coalesced area_covers_area + area_covers_point + area_inside_area +
  //   area_intersects_area filters
  // - collocate_left prefilter + exclude/keep wildcards + trailing sort/set
  const std::string ops = absl::StrFormat(
      "scalar_num == 42; scalar_str == \"alpha\"; "
      "status_flag >= 0; pressure > 900[hPa]; pressure <= 1000[hPa]; "
      "vertical_grid >= 100[m]; "
      "altitude > 0[m]; altitude < 1000[m]; "
      "level_label != \"bad\"; "
      "vertical_grid >= 100[m]; "
      "index(time) >= 0; index(vertical) in (0, 1); "
      "point_distance(0.1, 0.1, 100[km]); "
      "point_in_area((0,0,10,10),(0,10,10,0)); "
      "area_covers_point(5.0, 5.0); "
      "area_inside_area(\"%s\"); "
      "area_intersects_area(\"%s\"); "
      "exclude(excluded_var); "
      "exclude(indep_axis); "
      "keep(*); "
      "collocate_left(\"%s\")",
      area_file.c_str(), area_file.c_str(), colloc_file.c_str());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_ingest(xml_file.c_str(), ops.c_str(),
                                      "mode=good;extra=one", &raw_prod))
      << harp_errno_to_string(harp_errno);
  ScopedHarpProduct prod(raw_prod);
  // Samples t=0 and t=1 pass every filter (t=2 fails altitude>0,
  // t=3,4 fail point/polygon filters). Vertical indices 0 and 2 pass.
  EXPECT_EQ(2, prod->dimension[harp_dimension_time]);
  EXPECT_EQ(2, prod->dimension[harp_dimension_vertical]);

  // Verify non-time [vertical] and [latitude, vertical] variables were expanded
  // with time dimension when 2D vertical mask was present.
  harp_variable* vgrid = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "vertical_grid", &vgrid));
  EXPECT_EQ(2, vgrid->num_dimensions);
  EXPECT_EQ(harp_dimension_time, vgrid->dimension_type[0]);
  EXPECT_EQ(harp_dimension_vertical, vgrid->dimension_type[1]);

  harp_variable* lgrid = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "lat_vertical_field", &lgrid));
  EXPECT_EQ(3, lgrid->num_dimensions);
  EXPECT_EQ(harp_dimension_time, lgrid->dimension_type[0]);

  harp_variable* lbl = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(prod.get(), "level_label", &lbl));
  ASSERT_EQ(4L, lbl->num_elements);
  EXPECT_STREQ("ok0", lbl->data.string_data[0]);
  EXPECT_STREQ("ok0", lbl->data.string_data[1]);
  EXPECT_STREQ("okN", lbl->data.string_data[2]);
  EXPECT_STREQ("okN", lbl->data.string_data[3]);

  // 4. Exercise 2D filter creating initial time_mask (level_label) and
  // trailing in-memory ops (sort/derive) via evaluate_ingestion_mask(), plus
  // area_covers_area filter.
  const std::string ops_no_time_mask = absl::StrFormat(
      "level_label != \"bad\"; area_covers_area((2,8),(2,8)); "
      "sort(datetime); derive(latitude [rad])");
  raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_ingest(xml_file.c_str(), ops_no_time_mask.c_str(), nullptr,
                        &raw_prod));
  ScopedHarpProduct prod2(raw_prod);
  EXPECT_EQ(2, prod2->dimension[harp_dimension_time]);

  // 5. Exercise single-element block/full read paths and read_range -> read_all
  // without masks.
  raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_ingest(xml_file.c_str(), nullptr, nullptr, &raw_prod));
  ScopedHarpProduct prod_full(raw_prod);
  EXPECT_EQ(5, prod_full->dimension[harp_dimension_time]);

  // 6. Exercise filters that empty the product (product_mask == 0), ensuring
  // subsequent filters short-circuit cleanly and an empty product is returned.
  for (const char* empty_op : {
           "scalar_num == 999; pressure > 0[hPa]",
           "scalar_str == \"missing\"; index(time) == 0",
           "status_flag >= 0; pressure > 99999[hPa]; point_distance(0, 0, "
           "10[km])",
           "index(time) > 100; scalar_num == 42",
           "point_distance(80, 80, 1[km])",
           "area_covers_point(80, 80)",
       }) {
    raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_ingest(xml_file.c_str(), empty_op, nullptr, &raw_prod))
        << empty_op;
    ScopedHarpProduct empty_p(raw_prod);
    EXPECT_EQ(1, harp_product_is_empty(empty_p.get())) << empty_op;
  }

  // 7. Scalar lat/lon and 1D bounds expansion inside point/polygon filters.
  cfg.product_variant = 1;
  raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_ingest(xml_file.c_str(),
                                      "point_distance(5, 5, 50[km]); "
                                      "area_covers_point(5, 5)",
                                      nullptr, &raw_prod));
  ScopedHarpProduct scalar_p(raw_prod);
  EXPECT_EQ(5, scalar_p->dimension[harp_dimension_time]);
  cfg.product_variant = 0;

  // 8. 1D string value filter with num_times == 1 (upstream
  // execute_value_filter() in harp-ingestion.c:1079 omits
  // read_buffer_free_string_data() per iteration, so num_times == 1 frees
  // the single string cleanly in read_buffer_delete()).
  cfg.num_times = 1;
  raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_ingest(xml_file.c_str(), "tag == \"keep_a\"",
                                      nullptr, &raw_prod));
  ScopedHarpProduct str1d_p(raw_prod);
  EXPECT_EQ(1, str1d_p->dimension[harp_dimension_time]);
  cfg.num_times = 5;

  // 9. Wildcard keep() exercise (covers non-exact variable name matching).
  raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_ingest(xml_file.c_str(), "keep(pres*)", nullptr, &raw_prod));
  ScopedHarpProduct keep_wild_p(raw_prod);
  EXPECT_EQ(1, harp_product_has_variable(keep_wild_p.get(), "pressure"));
}

TEST(HarpIngestionTest, FilterAndReadErrorPaths) {
  const TempDir dir("harp_ingestion_synth_errors");
  const TempFile xml_file("harp_ingestion_synth_errors/product.xml");
  SyntheticConfig cfg;
  g_synth_cfg = &cfg;
  auto clear_cfg = absl::MakeCleanup([] { g_synth_cfg = nullptr; });

  WriteSyntheticCodadefAndXml(dir, xml_file);
  ASSERT_EQ(HARP_SUCCESS, harp_set_coda_definition_path(dir.c_str()));
  auto reset_coda =
      absl::MakeCleanup([] { harp_set_coda_definition_path(nullptr); });
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_init());
  RegisterSyntheticModule(/*with_options=*/true);

  harp_product* raw_prod = nullptr;

  // 1. Option validation errors and module init failure.
  EXPECT_EQ(-1,
            harp_ingest(xml_file.c_str(), nullptr, "no_such_opt=1", &raw_prod));
  EXPECT_EQ(HARP_ERROR_INVALID_INGESTION_OPTION, harp_errno);
  EXPECT_EQ(-1,
            harp_ingest(xml_file.c_str(), nullptr, "mode=invalid", &raw_prod));
  EXPECT_EQ(HARP_ERROR_INVALID_INGESTION_OPTION_VALUE, harp_errno);
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), nullptr, "mode=bad", &raw_prod));
  EXPECT_EQ(HARP_ERROR_INGESTION, harp_errno);

  cfg.fail_init = true;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), nullptr, nullptr, &raw_prod));
  cfg.fail_init = false;

  // 2. read_dimensions failure.
  cfg.fail_read_dimensions = true;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), nullptr, nullptr, &raw_prod));
  cfg.fail_read_dimensions = false;

  // 3. Invalid filter operations (non-existent var, keep missing var,
  // invalid dim for value filter, missing lat/lon/bounds, invalid dims).
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "no_such_var == 1", nullptr,
                            &raw_prod));
  EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);

  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "keep(no_such_var)", nullptr,
                            &raw_prod));
  EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);

  EXPECT_EQ(
      -1, harp_ingest(xml_file.c_str(), "indep_axis > 0", nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);

  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "lat_vertical_field > 0", nullptr,
                            &raw_prod));
  EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);

  // Unit incompatibility in value filter.
  EXPECT_EQ(
      -1, harp_ingest(xml_file.c_str(), "pressure > 1[m]", nullptr, &raw_prod));

  // Missing latitude / longitude for point filter.
  cfg.include_lat_lon = false;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "point_distance(0, 0, 10[km])",
                            nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);
  cfg.include_lat_lon = true;

  // Invalid latitude dimension for point filter.
  cfg.product_variant = 2;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "point_distance(0, 0, 10[km])",
                            nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);
  cfg.product_variant = 0;

  // Unconvertible latitude unit for point filter.
  cfg.product_variant = 5;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "point_distance(0, 0, 10[km])",
                            nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);
  cfg.product_variant = 0;

  // Missing / invalid / mismatched / short bounds for polygon filter.
  cfg.include_bounds = false;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "area_covers_point(5, 5)",
                            nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);
  cfg.include_bounds = true;

  cfg.product_variant = 2;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "area_covers_point(5, 5)",
                            nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);
  cfg.product_variant = 0;

  cfg.product_variant = 3;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "area_covers_point(5, 5)",
                            nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  cfg.product_variant = 0;

  cfg.product_variant = 4;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "area_covers_point(5, 5)",
                            nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  cfg.product_variant = 0;

  // 4. Read failures during filter evaluation and get_variable().
  cfg.fail_read_scalar = true;
  EXPECT_EQ(
      -1, harp_ingest(xml_file.c_str(), "scalar_num == 1", nullptr, &raw_prod));
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), nullptr, nullptr, &raw_prod));
  cfg.fail_read_scalar = false;

  cfg.fail_read_1d = true;
  EXPECT_EQ(-1,
            harp_ingest(xml_file.c_str(), "latitude > 0", nullptr, &raw_prod));
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "point_distance(0, 0, 10[km])",
                            nullptr, &raw_prod));
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "index(time) == 0", nullptr,
                            &raw_prod));
  cfg.fail_read_1d = false;

  cfg.fail_read_2d = true;
  EXPECT_EQ(-1,
            harp_ingest(xml_file.c_str(), "status_flag >= 0; altitude > 0[m]",
                        nullptr, &raw_prod));
  cfg.fail_read_2d = false;

  cfg.fail_read_range = true;
  EXPECT_EQ(-1,
            harp_ingest(xml_file.c_str(), "status_flag >= 0; index(time) == 0",
                        nullptr, &raw_prod));
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), nullptr, nullptr, &raw_prod));
  cfg.fail_read_range = false;

  cfg.fail_read_vertical_1d = true;
  EXPECT_EQ(-1,
            harp_ingest(xml_file.c_str(), "status_flag >= 0; altitude > 0[m]",
                        nullptr, &raw_prod));
  // 1D filter read_block fail when vertical already has a 2D mask.
  EXPECT_EQ(-1, harp_ingest(
                    xml_file.c_str(),
                    "status_flag >= 0; altitude > 0[m]; vertical_grid >= 0[m]",
                    nullptr, &raw_prod));
  cfg.fail_read_vertical_1d = false;

  // 5. Missing/failing longitude, unit/dim errors on longitude and bounds,
  // degenerate polygon, secondary-mask read_block failure, collocation errors,
  // verify() failure, and execute_program() failure.
  cfg.include_lon = false;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "point_distance(0, 0, 10[km])",
                            nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);
  cfg.include_lon = true;

  cfg.fail_read_lon_block = true;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), nullptr, nullptr, &raw_prod));
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "point_distance(0, 0, 10[km])",
                            nullptr, &raw_prod));
  cfg.fail_read_lon_block = false;

  cfg.include_lon_bounds = false;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "area_covers_point(5, 5)",
                            nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);
  cfg.include_lon_bounds = true;

  cfg.fail_read_lat_bounds = true;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "area_covers_point(5, 5)",
                            nullptr, &raw_prod));
  cfg.fail_read_lat_bounds = false;

  cfg.fail_read_lon_bounds = true;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "area_covers_point(5, 5)",
                            nullptr, &raw_prod));
  cfg.fail_read_lon_bounds = false;

  cfg.fail_read_label_2d = true;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(),
                            "status_flag >= 0; index(vertical) == 0", nullptr,
                            &raw_prod));
  cfg.fail_read_label_2d = false;

  cfg.flip_include_lat = true;
  cfg.include_lat_calls = 0;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "point_distance(0, 0, 10[km])",
                            nullptr, &raw_prod));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  cfg.flip_include_lat = false;

  for (int bad_variant : {6, 7}) {
    cfg.product_variant = bad_variant;
    EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "point_distance(0, 0, 10[km])",
                              nullptr, &raw_prod));
  }
  for (int bad_variant : {8, 9, 10, 11}) {
    cfg.product_variant = bad_variant;
    EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "area_covers_point(5, 5)",
                              nullptr, &raw_prod));
  }
  cfg.product_variant = 0;

  // Collocation filter errors (missing file, missing index variable).
  const TempFile missing_colloc(
      "harp_ingestion_synth_errors/missing_colloc.csv");
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(),
                            absl::StrFormat("collocate_left(\"%s\")",
                                            missing_colloc.c_str())
                                .c_str(),
                            nullptr, &raw_prod));

  const TempFile valid_colloc("harp_ingestion_synth_errors/valid_colloc.csv");
  {
    harp_collocation_result* raw_colloc = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_collocation_result_new(&raw_colloc, 0, nullptr, nullptr));
    ScopedHarpCollocationResult colloc(raw_colloc);
    const std::string src_a = xml_file.path().filename().string();
    ASSERT_EQ(HARP_SUCCESS,
              harp_collocation_result_add_pair(colloc.get(), 1, src_a.c_str(),
                                               0, "b.nc", 0, 0, nullptr));
    ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_write(valid_colloc.c_str(),
                                                          colloc.get()));
  }
  cfg.include_index = false;
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(),
                            absl::StrFormat("collocate_left(\"%s\")",
                                            valid_colloc.c_str())
                                .c_str(),
                            nullptr, &raw_prod));
  cfg.include_index = true;

  // Product verification failure (invalid unit), value-filter type-mismatch
  // eval() failures (scalar, 1D with 2D mask, and 2D), and execute_program
  // failure (derive non-existent variable).
  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "scalar_num == \"alpha\"",
                            nullptr, &raw_prod));
  EXPECT_EQ(
      -1,
      harp_ingest(xml_file.c_str(),
                  "status_flag >= 0; altitude > 0[m]; vertical_grid == \"bad\"",
                  nullptr, &raw_prod));
  EXPECT_EQ(
      -1, harp_ingest(xml_file.c_str(), "status_flag >= 0; altitude == \"bad\"",
                      nullptr, &raw_prod));

  for (int bad_variant : {12, 13}) {
    cfg.product_variant = bad_variant;
    EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), nullptr, nullptr, &raw_prod));
  }
  cfg.product_variant = 0;

  EXPECT_EQ(-1, harp_ingest(xml_file.c_str(), "derive(no_such_derived_var)",
                            nullptr, &raw_prod));
}

TEST(HarpIngestionTest, EmptyProductsMetadataVariantsAndIngestTestOptions) {
  const TempDir dir("harp_ingestion_synth_meta");
  const TempFile xml_file("harp_ingestion_synth_meta/product.xml");
  SyntheticConfig cfg;
  cfg.register_datetime_range = false;  // Exercise datetime* fallback path.
  g_synth_cfg = &cfg;
  auto clear_cfg = absl::MakeCleanup([] { g_synth_cfg = nullptr; });

  WriteSyntheticCodadefAndXml(dir, xml_file);
  ASSERT_EQ(HARP_SUCCESS, harp_set_coda_definition_path(dir.c_str()));
  auto reset_coda =
      absl::MakeCleanup([] { harp_set_coda_definition_path(nullptr); });
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_init());
  RegisterSyntheticModule(/*with_options=*/true);

  // 1. Metadata fallback reading datetime variable when read_datetime_range is
  // NULL.
  {
    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    ASSERT_EQ(HARP_SUCCESS,
              harp_ingest_metadata(xml_file.c_str(), "mode=good;extra=one",
                                   meta.get()));
    EXPECT_DOUBLE_EQ(100.0, meta->datetime_start);
    EXPECT_DOUBLE_EQ(104.0, meta->datetime_stop);
  }

  // 2. Empty dimension (vertical == 0) produces empty product and +/-inf
  // datetime metadata; index(vertical) filter also handles dimension <= 0
  // when no included variable uses vertical.
  cfg.empty_vertical_dim = true;
  cfg.include_vertical_vars = false;
  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_ingest(xml_file.c_str(), "index(vertical) == 0",
                                      nullptr, &raw_prod));
  ScopedHarpProduct empty_indep_dim_prod(raw_prod);
  EXPECT_EQ(5, empty_indep_dim_prod->dimension[harp_dimension_time]);
  cfg.include_vertical_vars = true;

  raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_ingest(xml_file.c_str(), "index(vertical) == 0",
                                      nullptr, &raw_prod));
  ScopedHarpProduct empty_dim_prod(raw_prod);
  EXPECT_EQ(1, harp_product_is_empty(empty_dim_prod.get()));

  {
    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    ASSERT_EQ(HARP_SUCCESS,
              harp_ingest_metadata(xml_file.c_str(), nullptr, meta.get()));
    EXPECT_EQ(1, harp_ismininf(meta->datetime_start));
    EXPECT_EQ(1, harp_isplusinf(meta->datetime_stop));
  }
  cfg.empty_vertical_dim = false;

  // 3. All variables excluded -> !product_has_variables -> empty product and
  // +/-inf datetime metadata.
  cfg.exclude_all_vars = true;
  raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_ingest(xml_file.c_str(), nullptr, nullptr, &raw_prod));
  ScopedHarpProduct no_vars_prod(raw_prod);
  EXPECT_EQ(1, harp_product_is_empty(no_vars_prod.get()));

  {
    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    ASSERT_EQ(HARP_SUCCESS,
              harp_ingest_metadata(xml_file.c_str(), nullptr, meta.get()));
    EXPECT_EQ(1, harp_ismininf(meta->datetime_start));
    EXPECT_EQ(1, harp_isplusinf(meta->datetime_stop));
  }
  cfg.exclude_all_vars = false;

  // 4. Metadata error paths (invalid option, init fail, datetime read fail,
  // missing datetime variable).
  {
    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    EXPECT_EQ(-1, harp_ingest_metadata(xml_file.c_str(), "no_such_opt=1",
                                       meta.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_INGESTION_OPTION, harp_errno);
  }
  cfg.fail_init = true;
  {
    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    EXPECT_EQ(-1, harp_ingest_metadata(xml_file.c_str(), nullptr, meta.get()));
  }
  cfg.fail_init = false;
  cfg.fail_read_datetime = true;
  {
    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    EXPECT_EQ(-1, harp_ingest_metadata(xml_file.c_str(), nullptr, meta.get()));
  }
  cfg.fail_read_datetime = false;
  cfg.include_datetime = false;
  {
    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    EXPECT_EQ(-1, harp_ingest_metadata(xml_file.c_str(), nullptr, meta.get()));
  }
  cfg.include_datetime = true;

  // 5. harp_ingest_test iterates all option combinations (unset, good/bad,
  // one/two); mode=bad fails so harp_ingest_test returns 1.
  EXPECT_EQ(1, harp_ingest_test(xml_file.c_str(), CapturePrint));
}

TEST(HarpIngestionTest, MetadataReadDatetimeRangeSuccessAndError) {
  const TempDir dir("harp_ingestion_synth_dtrange");
  const TempFile xml_file("harp_ingestion_synth_dtrange/product.xml");
  SyntheticConfig cfg;
  cfg.register_datetime_range = true;
  g_synth_cfg = &cfg;
  auto clear_cfg = absl::MakeCleanup([] { g_synth_cfg = nullptr; });

  WriteSyntheticCodadefAndXml(dir, xml_file);
  ASSERT_EQ(HARP_SUCCESS, harp_set_coda_definition_path(dir.c_str()));
  auto reset_coda =
      absl::MakeCleanup([] { harp_set_coda_definition_path(nullptr); });
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_init());
  RegisterSyntheticModule(/*with_options=*/false);

  {
    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    ASSERT_EQ(HARP_SUCCESS,
              harp_ingest_metadata(xml_file.c_str(), nullptr, meta.get()));
    EXPECT_DOUBLE_EQ(100.0, meta->datetime_start);
    EXPECT_DOUBLE_EQ(104.0, meta->datetime_stop);
  }

  cfg.fail_read_datetime_range = true;
  {
    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    EXPECT_EQ(-1, harp_ingest_metadata(xml_file.c_str(), nullptr, meta.get()));
    EXPECT_EQ(HARP_ERROR_INGESTION, harp_errno);
  }
  cfg.fail_read_datetime_range = false;

  cfg.fail_read_dimensions = true;
  {
    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    EXPECT_EQ(-1, harp_ingest_metadata(xml_file.c_str(), nullptr, meta.get()));
  }
  cfg.fail_read_dimensions = false;

  // All combinations pass when with_options == false -> harp_ingest_test == 0.
  EXPECT_EQ(0, harp_ingest_test(xml_file.c_str(), CapturePrint));
}

}  // namespace
}  // namespace stcorp_harp
