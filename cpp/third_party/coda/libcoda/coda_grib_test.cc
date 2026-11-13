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
// Tests CODA's GRIB Edition 1 and Edition 2 backend (libcoda/coda-grib.c):
// recognition, reopening, closing, multi-message & mixed-edition streams,
// filler skipping, GRIB1 PDS/GDS/BMS/BDS, GRIB2 Sections 0-8, simple packing,
// IEEE raw coordinate arrays, IBM float conversion, and malformed inputs.
//
// google3 build caveats:
//   - Built on little-endian LP64 Linux (WORDS_BIGENDIAN branches in
//     coda-grib.c are compiled out).
//   - Upstream coda-grib.c:2342-2358 leaks `raw_data` (does not free it
//     after copying via coda_mem_raw_new()) for GRIB2 Section 2 (Local Use)
//     with section_size > 5; tests exercising non-empty GRIB2 local use wrap
//     the open call in an unconditional absl::LeakCheckDisabler.
//   - Upstream coda-grib.c:1540 (`sign << 31` in ibmfloat_to_iee754) and
//     lines 2486/2492/2502/2508 (`1 << 31` for negative GRIB2 lat/lon) trigger
//     UBSan signed shift overflow when invoked with negative values; tests use
//     positive IBM floats and non-negative GRIB2 coordinates to stay
//     UBSan-clean while executing those lines.
//   - Internal OOM branches (malloc/strdup == NULL) and the bitmask read-error
//     return at coda-grib.c:3031 (unreachable because Section 7 follows
//     Section 6 in the file) are not reachable without fault injection.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-grib.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-grib.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-grib-internal.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/grib_builder.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-bin.h"
#include "third_party/stcorp_coda/libcoda/coda-grib.h"
}  // extern "C"

#include "third_party/stcorp_coda/libcoda/coda.h"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;
using ::testing::FloatNear;
using ::testing::HasSubstr;

// Cursor navigation and assertion helpers to keep tests concise.
void GotoPath(coda_product* product, const char* path, coda_cursor* cursor) {
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(cursor, product));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(cursor, path)) << "path=" << path;
}

void ExpectUint8(coda_product* product, const char* path, uint8_t expected) {
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product, path, &cursor));
  uint8_t actual = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&cursor, &actual)) << path;
  EXPECT_EQ(expected, actual) << path;
}

void ExpectUint16(coda_product* product, const char* path, uint16_t expected) {
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product, path, &cursor));
  uint16_t actual = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&cursor, &actual)) << path;
  EXPECT_EQ(expected, actual) << path;
}

void ExpectInt16(coda_product* product, const char* path, int16_t expected) {
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product, path, &cursor));
  int16_t actual = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &actual)) << path;
  EXPECT_EQ(expected, actual) << path;
}

void ExpectUint32(coda_product* product, const char* path, uint32_t expected) {
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product, path, &cursor));
  uint32_t actual = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint32(&cursor, &actual)) << path;
  EXPECT_EQ(expected, actual) << path;
}

void ExpectInt32(coda_product* product, const char* path, int32_t expected) {
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product, path, &cursor));
  int32_t actual = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &actual)) << path;
  EXPECT_EQ(expected, actual) << path;
}

void ExpectFloat(coda_product* product, const char* path, float expected) {
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product, path, &cursor));
  float actual = -999.0f;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&cursor, &actual)) << path;
  EXPECT_FLOAT_EQ(expected, actual) << path;
}

void ExpectDouble(coda_product* product, const char* path, double expected) {
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product, path, &cursor));
  double actual = -999.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &actual)) << path;
  EXPECT_DOUBLE_EQ(expected, actual) << path;
}

void ExpectDoubleNaN(coda_product* product, const char* path) {
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product, path, &cursor));
  double actual = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &actual)) << path;
  EXPECT_TRUE(std::isnan(actual)) << path << " actual=" << actual;
}

void ExpectFieldAvailable(coda_product* product, const char* record_path,
                          const char* field_name, bool expected_available) {
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product, record_path, &cursor));
  long field_index = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_index_from_name(
                              &cursor, field_name, &field_index))
      << record_path << "/" << field_name;
  int available = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                              &cursor, field_index, &available))
      << record_path << "/" << field_name;
  EXPECT_EQ(expected_available ? 1 : 0, available)
      << record_path << "/" << field_name;
}

void ExpectArrayCount(coda_product* product, const char* path,
                      long expected_count) {
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product, path, &cursor));
  long num_elements = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elements))
      << path;
  EXPECT_EQ(expected_count, num_elements) << path;
}

void ReadFloatArray(coda_product* product, const char* path,
                    std::vector<float>* out) {
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product, path, &cursor));
  long num_elements = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elements))
      << path;
  ASSERT_GE(num_elements, 0) << path;
  out->assign(static_cast<size_t>(num_elements), -999.0f);
  if (num_elements > 0) {
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(&cursor, out->data(),
                                                         coda_array_ordering_c))
        << path;
  }
}

void ReadUint16Array(coda_product* product, const char* path,
                     std::vector<uint16_t>* out) {
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product, path, &cursor));
  long num_elements = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elements))
      << path;
  ASSERT_GE(num_elements, 0) << path;
  out->assign(static_cast<size_t>(num_elements), 0xFFFFu);
  if (num_elements > 0) {
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16_array(
                                &cursor, out->data(), coda_array_ordering_c))
        << path;
  }
}

void ReadUint32Array(coda_product* product, const char* path,
                     std::vector<uint32_t>* out) {
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product, path, &cursor));
  long num_elements = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elements))
      << path;
  ASSERT_GE(num_elements, 0) << path;
  out->assign(static_cast<size_t>(num_elements), 0xFFFFFFFFu);
  if (num_elements > 0) {
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint32_array(
                                &cursor, out->data(), coda_array_ordering_c))
        << path;
  }
}

void ReadRawBytes(coda_product* product, const char* path,
                  std::vector<uint8_t>* out) {
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product, path, &cursor));
  int64_t byte_size = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_byte_size(&cursor, &byte_size))
      << path;
  ASSERT_GE(byte_size, 0) << path;
  out->assign(static_cast<size_t>(byte_size), 0xAAu);
  if (byte_size > 0) {
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_bytes(&cursor, out->data(), 0, byte_size))
        << path;
  }
}

void ExpectGribOpenFails(std::string_view filename,
                         std::span<const uint8_t> bytes, int expected_errno,
                         std::string_view expected_substr) {
  TempFile file(filename);
  ASSERT_TRUE(file.WriteBytes(bytes));
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };
  ScopedProduct product;
  EXPECT_EQ(-1, coda_open(file.c_str(), product.OutParam())) << filename;
  EXPECT_EQ(expected_errno, coda_errno) << filename;
  EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr(expected_substr))
      << filename;
  EXPECT_EQ(nullptr, product.get()) << filename;
}

void ExpectGribReopenFails(std::string_view filename,
                           std::span<const uint8_t> bytes, int expected_errno,
                           std::string_view expected_substr) {
  TempFile file(filename);
  ASSERT_TRUE(file.WriteBytes(bytes));
  coda_product* raw_product = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_bin_open(file.c_str(), static_cast<int64_t>(bytes.size()),
                          &raw_product));
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };
  // coda_grib_reopen() closes raw_product on failure.
  const int status = coda_grib_reopen(&raw_product);
  if (status == 0) {
    coda_close(raw_product);
  }
  EXPECT_EQ(-1, status) << filename;
  EXPECT_EQ(expected_errno, coda_errno) << filename;
  EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr(expected_substr))
      << filename;
}

TEST(CodaGribTest, DoneWithoutOpenIsNoOp) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();

  // Direct coda_grib_done() when grib_type == NULL takes early return.
  coda_grib_done();

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  // ScopedCodaSession::Reset() invokes coda_done() -> coda_grib_done()
  // cleanly with no GRIB product opened.
}

TEST(CodaGribTest, RecognizeFileOnValidCorruptAndUnsupportedEditionGrib) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  // 1. Valid GRIB1 file is recognized as coda_format_grib.
  {
    const std::vector<uint8_t> grib1 =
        GribBuilder::BuildGrib1(Grib1MessageSpec{});
    TempFile file("grib_recognize_v1.grb");
    ASSERT_TRUE(file.WriteBytes(grib1));

    int64_t file_size = -1;
    coda_format format = coda_format_binary;
    const char* product_class = "sentinel";
    const char* product_type = "sentinel";
    int version = 99;
    ASSERT_EQ(CODA_SUCCESS,
              coda_recognize_file(file.c_str(), &file_size, &format,
                                  &product_class, &product_type, &version));
    EXPECT_EQ(static_cast<int64_t>(grib1.size()), file_size);
    EXPECT_EQ(coda_format_grib, format);
    EXPECT_EQ(nullptr, product_class);
    EXPECT_EQ(nullptr, product_type);
    EXPECT_EQ(-1, version);
  }

  // 2. Valid GRIB2 file is recognized as coda_format_grib.
  {
    const std::vector<uint8_t> grib2 =
        GribBuilder::BuildGrib2(Grib2MessageSpec{});
    TempFile file("grib_recognize_v2.grb2");
    ASSERT_TRUE(file.WriteBytes(grib2));

    int64_t file_size = -1;
    coda_format format = coda_format_binary;
    const char* product_class = "sentinel";
    const char* product_type = "sentinel";
    int version = 99;
    ASSERT_EQ(CODA_SUCCESS,
              coda_recognize_file(file.c_str(), &file_size, &format,
                                  &product_class, &product_type, &version));
    EXPECT_EQ(static_cast<int64_t>(grib2.size()), file_size);
    EXPECT_EQ(coda_format_grib, format);
    EXPECT_EQ(nullptr, product_class);
    EXPECT_EQ(nullptr, product_type);
    EXPECT_EQ(-1, version);
  }

  // 3. Corrupt GRIB1 (truncated after Section 0) fails recognition.
  {
    const std::vector<uint8_t> trunc_v1 = {'G', 'R', 'I', 'B', 0, 0,
                                           20,  1,   0,   0,   28};
    TempFile file("grib_recognize_corrupt_v1.grb");
    ASSERT_TRUE(file.WriteBytes(trunc_v1));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_recognize_file(file.c_str(), nullptr, nullptr, nullptr,
                                      nullptr, nullptr));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }

  // 4. Corrupt GRIB2 (Section 0 + Section 1 + '7777', no data) fails.
  {
    Grib2MessageSpec empty_msg;
    empty_msg.omit_data_sections = true;
    const std::vector<uint8_t> corrupt_v2 = GribBuilder::BuildGrib2(empty_msg);
    TempFile file("grib_recognize_corrupt_v2.grb2");
    ASSERT_TRUE(file.WriteBytes(corrupt_v2));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_recognize_file(file.c_str(), nullptr, nullptr, nullptr,
                                      nullptr, nullptr));
    EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("Message contains no data"));
  }

  // 5. Unsupported edition at offset 0 falls back to coda_format_binary in
  //    coda-product.c get_format(), whereas edition 3 on message 1 triggers
  //    CODA_ERROR_UNSUPPORTED_PRODUCT in coda_grib_reopen().
  {
    for (const uint8_t bad_ed : {uint8_t{0}, uint8_t{3}}) {
      const std::vector<uint8_t> hdr = {'G', 'R',    'I', 'B', 0,   0,
                                        12,  bad_ed, '7', '7', '7', '7'};
      TempFile file(absl::StrCat("grib_ed", bad_ed, "_at0.grb"));
      ASSERT_TRUE(file.WriteBytes(hdr));
      coda_format format = coda_format_grib;
      ASSERT_EQ(CODA_SUCCESS,
                coda_recognize_file(file.c_str(), nullptr, &format, nullptr,
                                    nullptr, nullptr));
      EXPECT_EQ(coda_format_binary, format);
    }

    GribBuilder builder;
    builder.AddGrib1(Grib1MessageSpec{});
    Grib1MessageSpec bad_second;
    bad_second.edition_override = 3;
    builder.AddGrib1(bad_second);
    TempFile file("grib_recognize_bad_second_ed.grb");
    ASSERT_TRUE(file.WriteBytes(builder.Build()));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_recognize_file(file.c_str(), nullptr, nullptr, nullptr,
                                      nullptr, nullptr));
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("unsupported GRIB format version (3) for message 1"));
  }

  // 6. Missing file fails with CODA_ERROR_FILE_NOT_FOUND.
  {
    const std::filesystem::path missing =
        std::filesystem::path(::testing::TempDir()) / "grib_missing.grb";
    std::filesystem::remove(missing);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_recognize_file(missing.c_str(), nullptr, nullptr,
                                      nullptr, nullptr, nullptr));
    EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
  }
}

TEST(CodaGribTest, Grib1PdsWithAndWithoutLocalAndDateFields) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  GribBuilder builder;

  // Message 0: 28-byte PDS (no local extension), positive decimalScaleFactor.
  Grib1MessageSpec msg0;
  msg0.table2_version = 128;
  msg0.centre = 7;
  msg0.generating_process_id = 81;
  msg0.grid_definition = 255;
  msg0.parameter = 130;
  msg0.level_type = 100;
  msg0.level = 500;
  msg0.year_of_century = 25;
  msg0.month = 11;
  msg0.day = 19;
  msg0.hour = 18;
  msg0.minute = 30;
  msg0.unit_of_time_range = 1;
  msg0.p1 = 6;
  msg0.p2 = 12;
  msg0.time_range_indicator = 4;
  msg0.number_included_in_average = 30;
  msg0.number_missing_from_averages = 2;
  msg0.century = 21;
  msg0.sub_centre = 5;
  msg0.decimal_scale_factor = 2;
  builder.AddGrib1(msg0);

  // Message 1: 32-byte PDS (28 < section_size <= 40 -> skips reserved bytes,
  // no `local` field emitted).
  Grib1MessageSpec msg1;
  msg1.pds_reserved_bytes = {1, 2, 3, 4};
  builder.AddGrib1(msg1);

  // Message 2: >40-byte PDS with local extension and negative decimal scale.
  Grib1MessageSpec msg2;
  msg2.decimal_scale_factor = -3;
  msg2.pds_reserved_bytes = {0x10, 0x20};
  msg2.local_use = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
  builder.AddGrib1(msg2);

  const std::vector<uint8_t> bytes = builder.Build();
  TempFile file("grib1_pds.grb");
  ASSERT_TRUE(file.WriteBytes(bytes));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

  coda_format fmt = coda_format_binary;
  ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(product.get(), &fmt));
  EXPECT_EQ(coda_format_grib, fmt);

  ExpectArrayCount(product.get(), "/", 3);

  // Verify union selection for GRIB1.
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product.get(), "/[0]", &cursor));
  long union_idx = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_available_union_field_index(&cursor, &union_idx));
  EXPECT_EQ(0, union_idx);

  // Verify Message 0 PDS fields.
  ExpectUint8(product.get(), "/[0]/grib1/editionNumber", 1);
  ExpectUint8(product.get(), "/[0]/grib1/table2Version", 128);
  ExpectUint8(product.get(), "/[0]/grib1/centre", 7);
  ExpectUint8(product.get(), "/[0]/grib1/generatingProcessIdentifier", 81);
  ExpectUint8(product.get(), "/[0]/grib1/gridDefinition", 255);
  ExpectUint8(product.get(), "/[0]/grib1/indicatorOfParameter", 130);
  ExpectUint8(product.get(), "/[0]/grib1/indicatorOfTypeOfLevel", 100);
  ExpectUint16(product.get(), "/[0]/grib1/level", 500);
  ExpectUint8(product.get(), "/[0]/grib1/yearOfCentury", 25);
  ExpectUint8(product.get(), "/[0]/grib1/month", 11);
  ExpectUint8(product.get(), "/[0]/grib1/day", 19);
  ExpectUint8(product.get(), "/[0]/grib1/hour", 18);
  ExpectUint8(product.get(), "/[0]/grib1/minute", 30);
  ExpectUint8(product.get(), "/[0]/grib1/unitOfTimeRange", 1);
  ExpectUint8(product.get(), "/[0]/grib1/P1", 6);
  ExpectUint8(product.get(), "/[0]/grib1/P2", 12);
  ExpectUint8(product.get(), "/[0]/grib1/timeRangeIndicator", 4);
  ExpectUint16(product.get(), "/[0]/grib1/numberIncludedInAverage", 30);
  ExpectUint8(product.get(),
              "/[0]/grib1/numberMissingFromAveragesOrAccumulations", 2);
  ExpectUint8(product.get(), "/[0]/grib1/centuryOfReferenceTimeOfData", 21);
  ExpectUint8(product.get(), "/[0]/grib1/subCentre", 5);
  ExpectInt16(product.get(), "/[0]/grib1/decimalScaleFactor", 2);
  ExpectFieldAvailable(product.get(), "/[0]/grib1", "local", false);
  ExpectFieldAvailable(product.get(), "/[0]/grib1", "grid", true);

  // Verify Message 1 (32-byte PDS -> `local` absent).
  ExpectFieldAvailable(product.get(), "/[1]/grib1", "local", false);

  // Verify Message 2 (>40-byte PDS -> `local` present and negative decimal).
  ExpectInt16(product.get(), "/[2]/grib1/decimalScaleFactor", -3);
  ExpectFieldAvailable(product.get(), "/[2]/grib1", "local", true);
  std::vector<uint8_t> local_bytes;
  ASSERT_NO_FATAL_FAILURE(
      ReadRawBytes(product.get(), "/[2]/grib1/local", &local_bytes));
  EXPECT_THAT(local_bytes, ElementsAre(0xDE, 0xAD, 0xBE, 0xEF, 0x42));
}

TEST(CodaGribTest, Grib1GdsGridTypesVerticalCoordsAndReducedGrids) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  GribBuilder builder;

  // Message 0: Lat/Lon (type 0) with negative lat/lon and vertical coords.
  Grib1MessageSpec latlon_msg;
  latlon_msg.gds = Grib1GdsSpec{
      .data_representation_type = 0,
      .ni = 2,
      .nj = 3,
      .latitude_first = -15000,
      .longitude_first = -30000,
      .resolution_and_component_flags = 0x88,
      .latitude_last = 45000,
      .longitude_last = 60000,
      .i_direction_increment = 30000,
      .j_direction_increment_or_n = 30000,
      .scanning_mode = 0x40,
      .coordinate_values = {1.0f, 2.5f, 0.0f},
  };
  builder.AddGrib1(latlon_msg);

  // Message 1: Gaussian (type 4) with PVL=255 and extra_template_bytes (>32).
  Grib1MessageSpec gauss_msg;
  gauss_msg.gds = Grib1GdsSpec{
      .data_representation_type = 4,
      .ni = 2,
      .nj = 2,
      .j_direction_increment_or_n = 160,
      .extra_template_bytes = {0xAA, 0xBB, 0xCC, 0xDD},
      .pvl_override = 255,
  };
  builder.AddGrib1(gauss_msg);

  // Message 2: Reduced grid with Ni=65535 (missing), listOfNumbers for Nj=3.
  Grib1MessageSpec reduced_ni_msg;
  reduced_ni_msg.gds = Grib1GdsSpec{
      .data_representation_type = 14,
      .ni = 65535,
      .nj = 3,
      .j_direction_increment_or_n = 48,
      .list_of_numbers = {2, 4, 2},
  };
  builder.AddGrib1(reduced_ni_msg);

  // Message 3: Reduced grid with Nj=65535 (missing), coordinateValues + list.
  Grib1MessageSpec reduced_nj_msg;
  reduced_nj_msg.gds = Grib1GdsSpec{
      .data_representation_type = 20,
      .ni = 2,
      .nj = 65535,
      .j_direction_increment_or_n = 500,
      .coordinate_values = {10.0f},
      .list_of_numbers = {3, 1},
  };
  builder.AddGrib1(reduced_nj_msg);

  // Messages 4..7: Remaining supported GRIB1 grid types (10, 24, 30, 34).
  for (const uint8_t drt :
       {uint8_t{10}, uint8_t{24}, uint8_t{30}, uint8_t{34}}) {
    Grib1MessageSpec msg;
    msg.gds = Grib1GdsSpec{
        .data_representation_type = drt,
        .ni = 2,
        .nj = 1,
        .j_direction_increment_or_n = 80,
    };
    builder.AddGrib1(msg);
  }

  TempFile file("grib1_gds.grb");
  ASSERT_TRUE(file.WriteBytes(builder.Build()));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

  // Message 0 assertions.
  ExpectUint8(product.get(), "/[0]/grib1/grid/numberOfVerticalCoordinateValues",
              3);
  ExpectUint8(product.get(), "/[0]/grib1/grid/dataRepresentationType", 0);
  ExpectUint16(product.get(), "/[0]/grib1/grid/Ni", 2);
  ExpectUint16(product.get(), "/[0]/grib1/grid/Nj", 3);
  ExpectInt32(product.get(), "/[0]/grib1/grid/latitudeOfFirstGridPoint",
              -15000);
  ExpectInt32(product.get(), "/[0]/grib1/grid/longitudeOfFirstGridPoint",
              -30000);
  ExpectUint8(product.get(), "/[0]/grib1/grid/resolutionAndComponentFlags",
              0x88);
  ExpectInt32(product.get(), "/[0]/grib1/grid/latitudeOfLastGridPoint", 45000);
  ExpectInt32(product.get(), "/[0]/grib1/grid/longitudeOfLastGridPoint", 60000);
  ExpectUint16(product.get(), "/[0]/grib1/grid/iDirectionIncrement", 30000);
  ExpectUint16(product.get(), "/[0]/grib1/grid/jDirectionIncrement", 30000);
  ExpectUint8(product.get(), "/[0]/grib1/grid/scanningMode", 0x40);
  ExpectFieldAvailable(product.get(), "/[0]/grib1/grid", "N", false);
  ExpectFieldAvailable(product.get(), "/[0]/grib1/grid", "coordinateValues",
                       true);
  ExpectFieldAvailable(product.get(), "/[0]/grib1/grid", "listOfNumbers",
                       false);
  std::vector<float> coords0;
  ASSERT_NO_FATAL_FAILURE(ReadFloatArray(
      product.get(), "/[0]/grib1/grid/coordinateValues", &coords0));
  EXPECT_THAT(coords0, ElementsAre(FloatNear(1.0f, 1e-5f),
                                   FloatNear(2.5f, 1e-5f), 0.0f));
  ExpectArrayCount(product.get(), "/[0]/grib1/data/values", 6);

  // Message 1 (Gaussian, PVL=255).
  ExpectUint8(product.get(), "/[1]/grib1/grid/dataRepresentationType", 4);
  ExpectUint16(product.get(), "/[1]/grib1/grid/N", 160);
  ExpectFieldAvailable(product.get(), "/[1]/grib1/grid", "jDirectionIncrement",
                       false);
  ExpectFieldAvailable(product.get(), "/[1]/grib1/grid", "coordinateValues",
                       false);

  // Message 2 (Reduced Gaussian, Ni=65535).
  ExpectUint16(product.get(), "/[2]/grib1/grid/N", 48);
  std::vector<uint16_t> list2;
  ASSERT_NO_FATAL_FAILURE(
      ReadUint16Array(product.get(), "/[2]/grib1/grid/listOfNumbers", &list2));
  EXPECT_THAT(list2, ElementsAre(2, 4, 2));
  ExpectArrayCount(product.get(), "/[2]/grib1/data/values", 8);

  // Message 3 (Reduced non-Gaussian, Nj=65535 + coordinateValues).
  ExpectUint16(product.get(), "/[3]/grib1/grid/jDirectionIncrement", 500);
  std::vector<float> coords3;
  ASSERT_NO_FATAL_FAILURE(ReadFloatArray(
      product.get(), "/[3]/grib1/grid/coordinateValues", &coords3));
  EXPECT_THAT(coords3, ElementsAre(FloatNear(10.0f, 1e-5f)));
  std::vector<uint16_t> list3;
  ASSERT_NO_FATAL_FAILURE(
      ReadUint16Array(product.get(), "/[3]/grib1/grid/listOfNumbers", &list3));
  EXPECT_THAT(list3, ElementsAre(3, 1));
  ExpectArrayCount(product.get(), "/[3]/grib1/data/values", 4);

  // Messages 4..7 (types 10, 24, 30, 34).
  ExpectFieldAvailable(product.get(), "/[4]/grib1/grid", "jDirectionIncrement",
                       true);
  ExpectFieldAvailable(product.get(), "/[5]/grib1/grid", "N", true);
  ExpectFieldAvailable(product.get(), "/[6]/grib1/grid", "jDirectionIncrement",
                       true);
  ExpectFieldAvailable(product.get(), "/[7]/grib1/grid", "N", true);
}

TEST(CodaGribTest, Grib1PredefinedGridsWithoutGds) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  struct PredefinedGridCase {
    uint8_t grid_id;
    long expected_elements;
  };
  constexpr PredefinedGridCase kPredefinedGrids[] = {
      {.grid_id = 1, .expected_elements = 1679},
      {.grid_id = 2, .expected_elements = 10512},
      {.grid_id = 3, .expected_elements = 65160},
      {.grid_id = 4, .expected_elements = 259920},
      {.grid_id = 5, .expected_elements = 2385},
      {.grid_id = 6, .expected_elements = 2385},
      {.grid_id = 8, .expected_elements = 5104},
      {.grid_id = 21, .expected_elements = 1333},
      {.grid_id = 22, .expected_elements = 1333},
      {.grid_id = 23, .expected_elements = 1333},
      {.grid_id = 24, .expected_elements = 1333},
      {.grid_id = 25, .expected_elements = 1297},
      {.grid_id = 26, .expected_elements = 1297},
      {.grid_id = 27, .expected_elements = 4225},
      {.grid_id = 28, .expected_elements = 4225},
      {.grid_id = 29, .expected_elements = 5365},
      {.grid_id = 30, .expected_elements = 5365},
      {.grid_id = 33, .expected_elements = 8326},
      {.grid_id = 34, .expected_elements = 8326},
      {.grid_id = 50, .expected_elements = 964},
      {.grid_id = 53, .expected_elements = 5967},
      {.grid_id = 55, .expected_elements = 6177},
      {.grid_id = 56, .expected_elements = 6177},
      {.grid_id = 61, .expected_elements = 4096},
      {.grid_id = 62, .expected_elements = 4096},
      {.grid_id = 63, .expected_elements = 4096},
      {.grid_id = 64, .expected_elements = 4096},
      {.grid_id = 75, .expected_elements = 12321},
      {.grid_id = 76, .expected_elements = 12321},
      {.grid_id = 77, .expected_elements = 12321},
      {.grid_id = 85, .expected_elements = 32400},
      {.grid_id = 86, .expected_elements = 32400},
      {.grid_id = 87, .expected_elements = 5022},
      {.grid_id = 90, .expected_elements = 12902},
      {.grid_id = 91, .expected_elements = 25803},
      {.grid_id = 92, .expected_elements = 81213},
      {.grid_id = 93, .expected_elements = 162425},
      {.grid_id = 94, .expected_elements = 48916},
      {.grid_id = 95, .expected_elements = 97831},
      {.grid_id = 96, .expected_elements = 41630},
      {.grid_id = 97, .expected_elements = 83259},
      {.grid_id = 100, .expected_elements = 6889},
      {.grid_id = 101, .expected_elements = 10283},
      {.grid_id = 103, .expected_elements = 3640},
      {.grid_id = 104, .expected_elements = 16170},
      {.grid_id = 105, .expected_elements = 6889},
      {.grid_id = 106, .expected_elements = 19305},
      {.grid_id = 107, .expected_elements = 11040},
  };

  GribBuilder builder;
  for (const PredefinedGridCase& tc : kPredefinedGrids) {
    Grib1MessageSpec msg;
    msg.grid_definition = tc.grid_id;
    msg.gds = std::nullopt;
    msg.bds.bits_per_value = 0;
    msg.bds.reference_value = 42.0f;
    builder.AddGrib1(msg);
  }

  TempFile file("grib1_predefined_grids.grb");
  ASSERT_TRUE(file.WriteBytes(builder.Build()));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

  for (size_t i = 0; i < std::size(kPredefinedGrids); ++i) {
    SCOPED_TRACE(kPredefinedGrids[i].grid_id);
    const std::string grid_path = absl::StrCat("/[", i, "]/grib1");
    const std::string val_path = absl::StrCat("/[", i, "]/grib1/data/values");
    ExpectFieldAvailable(product.get(), grid_path.c_str(), "grid", false);
    ExpectArrayCount(product.get(), val_path.c_str(),
                     kPredefinedGrids[i].expected_elements);
  }
  ExpectFloat(product.get(), "/[0]/grib1/data/values[0]", 42.0f);
}

TEST(CodaGribTest, Grib1BmsAndBdsSimplePackingAndIbmFloatEdgeCases) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  GribBuilder builder;

  // Message 0: Inline BMS (masked index 1) + simple packing with negative
  // binaryScaleFactor (-1) and positive decimalScaleFactor (1).
  Grib1MessageSpec masked_msg;
  masked_msg.decimal_scale_factor = 1;
  masked_msg.gds = Grib1GdsSpec{.ni = 2, .nj = 2};
  masked_msg.bms = Grib1BmsSpec{.bitmap = {true, false, true, true}};
  masked_msg.bds = Grib1BdsSpec{
      .flags = 0x20,  // Integer flag is accepted; 0x80/0x40/0x10 rejected.
      .binary_scale_factor = -1,
      .reference_value = 10.0f,
      .bits_per_value = 8,
      .packed_ints = {0, 2, 6},
      .unused_bits_override = 0,
  };
  builder.AddGrib1(masked_msg);

  // Message 1: Large BMS (>128 bits = 140 elements) exercising multi-block
  // bitmask_cumsum128 indexing across byte 16.
  Grib1MessageSpec large_bms_msg;
  large_bms_msg.gds = Grib1GdsSpec{.ni = 14, .nj = 10};
  std::vector<bool> mask140(140, false);
  mask140[0] = true;
  mask140[129] = true;
  mask140[139] = true;
  large_bms_msg.bms = Grib1BmsSpec{.bitmap = mask140};
  large_bms_msg.bds = Grib1BdsSpec{
      .reference_value = 5.0f,
      .bits_per_value = 8,
      .packed_ints = {1, 2, 3},
  };
  builder.AddGrib1(large_bms_msg);

  // Message 2: bitsPerValue == 0 (constant field equal to referenceValue) and
  // IBM float edge cases in coordinateValues (0.0, subnormal, Inf).
  Grib1MessageSpec ibm_msg;
  ibm_msg.gds = Grib1GdsSpec{
      .ni = 2,
      .nj = 1,
      .coordinate_values =
          {
              0.0f,
              1.0f,
              std::ldexp(1.0f, -140),
              std::numeric_limits<float>::infinity(),
          },
  };
  ibm_msg.bds = Grib1BdsSpec{
      .reference_value = 12.5f,
      .bits_per_value = 0,
  };
  builder.AddGrib1(ibm_msg);

  // Message 3: Non-zero unaligned 12-bit packing with unused_bits_override.
  Grib1MessageSpec bit12_msg;
  bit12_msg.decimal_scale_factor = -1;
  bit12_msg.gds = Grib1GdsSpec{.ni = 3, .nj = 1};
  bit12_msg.bds = Grib1BdsSpec{
      .binary_scale_factor = 1,
      .reference_value = 5.0f,
      .bits_per_value = 12,
      .packed_ints = {0, 1, 2},
      .unused_bits_override = 4,
  };
  builder.AddGrib1(bit12_msg);

  // Message 4: Patched tiny IBM float {0x01, 0x80, 0x00, 0x00} in BDS
  // referenceValue -> exponent < -24 underflow branch -> 0.0f.
  std::vector<uint8_t> underflow_msg = GribBuilder::BuildGrib1(Grib1MessageSpec{
      .gds = Grib1GdsSpec{.ni = 1, .nj = 1},
      .bds = Grib1BdsSpec{.bits_per_value = 0},
  });
  // Section 0 (8) + PDS (28) + GDS (32) + BDS header offset 6 = 74.
  ASSERT_GT(underflow_msg.size(), size_t{78});
  underflow_msg[74] = 0x01;
  underflow_msg[75] = 0x80;
  underflow_msg[76] = 0x00;
  underflow_msg[77] = 0x00;
  builder.AddFiller(underflow_msg);

  TempFile file("grib1_bms_bds.grb");
  ASSERT_TRUE(file.WriteBytes(builder.Build()));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

  // Message 0: masked values.
  ExpectUint8(product.get(), "/[0]/grib1/data/bitsPerValue", 8);
  ExpectInt16(product.get(), "/[0]/grib1/data/binaryScaleFactor", -1);
  ExpectFloat(product.get(), "/[0]/grib1/data/referenceValue", 10.0f);
  std::vector<float> vals0;
  ASSERT_NO_FATAL_FAILURE(
      ReadFloatArray(product.get(), "/[0]/grib1/data/values", &vals0));
  ASSERT_EQ(4u, vals0.size());
  EXPECT_FLOAT_EQ(1.0f, vals0[0]);
  EXPECT_TRUE(std::isnan(vals0[1]));
  EXPECT_FLOAT_EQ(1.1f, vals0[2]);
  EXPECT_FLOAT_EQ(1.3f, vals0[3]);

  // Message 1: >128-bit bitmap.
  std::vector<float> vals1;
  ASSERT_NO_FATAL_FAILURE(
      ReadFloatArray(product.get(), "/[1]/grib1/data/values", &vals1));
  ASSERT_EQ(140u, vals1.size());
  EXPECT_FLOAT_EQ(6.0f, vals1[0]);
  EXPECT_TRUE(std::isnan(vals1[1]));
  EXPECT_TRUE(std::isnan(vals1[128]));
  EXPECT_FLOAT_EQ(7.0f, vals1[129]);
  EXPECT_FLOAT_EQ(8.0f, vals1[139]);

  // Message 2: bitsPerValue == 0 and IBM float edge cases.
  std::vector<float> vals2;
  ASSERT_NO_FATAL_FAILURE(
      ReadFloatArray(product.get(), "/[2]/grib1/data/values", &vals2));
  EXPECT_THAT(vals2, ElementsAre(12.5f, 12.5f));
  std::vector<float> coords2;
  ASSERT_NO_FATAL_FAILURE(ReadFloatArray(
      product.get(), "/[2]/grib1/grid/coordinateValues", &coords2));
  ASSERT_EQ(4u, coords2.size());
  EXPECT_FLOAT_EQ(0.0f, coords2[0]);
  EXPECT_FLOAT_EQ(1.0f, coords2[1]);
  EXPECT_GT(coords2[2], 0.0f);
  EXPECT_TRUE(std::isinf(coords2[3]) && coords2[3] > 0.0f);

  // Message 3: 12-bit packing + partial array read.
  std::vector<float> vals3;
  ASSERT_NO_FATAL_FAILURE(
      ReadFloatArray(product.get(), "/[3]/grib1/data/values", &vals3));
  EXPECT_THAT(vals3, ElementsAre(50.0f, 70.0f, 90.0f));
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(
      GotoPath(product.get(), "/[3]/grib1/data/values", &cursor));
  std::array<float, 2> partial = {-1.0f, -1.0f};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_partial_array(&cursor, 1, 2,
                                                               partial.data()));
  EXPECT_THAT(partial, ElementsAre(70.0f, 90.0f));

  // Message 4: deep IBM underflow -> 0.0f.
  ExpectFloat(product.get(), "/[4]/grib1/data/referenceValue", 0.0f);
}

TEST(CodaGribTest, Grib1MalformedAndUnsupportedErrorPaths) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  struct ErrorCase {
    std::string_view name;
    std::vector<uint8_t> bytes;
    int expected_errno;
    std::string_view expected_substr;
  };
  std::vector<ErrorCase> cases;

  // 1. Truncated PDS (<28 bytes).
  cases.push_back({
      .name = "grib1_trunc_pds.grb",
      .bytes = {'G', 'R', 'I', 'B', 0, 0, 20, 1, 0, 0, 28, 3, 98},
      .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
      .expected_substr = "beyond the end of the file",
  });

  // 2. Truncated PDS local extension (section_size = 50, truncated at byte 42).
  {
    std::vector<uint8_t> b = GribBuilder::BuildGrib1(Grib1MessageSpec{
        .local_use = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
    });
    b.resize(8 + 42);
    cases.push_back({
        .name = "grib1_trunc_pds_local.grb",
        .bytes = std::move(b),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });
  }

  // 3. Truncated GDS header (6 bytes) and GDS body (26 bytes).
  {
    std::vector<uint8_t> b = GribBuilder::BuildGrib1(Grib1MessageSpec{});
    b.resize(8 + 28 + 3);
    cases.push_back({
        .name = "grib1_trunc_gds_hdr.grb",
        .bytes = std::move(b),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });

    std::vector<uint8_t> b2 = GribBuilder::BuildGrib1(Grib1MessageSpec{});
    b2.resize(8 + 28 + 12);
    cases.push_back({
        .name = "grib1_trunc_gds_body.grb",
        .bytes = std::move(b2),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });
  }

  // 4. Unsupported GDS dataRepresentationType.
  cases.push_back({
      .name = "grib1_bad_drt.grb",
      .bytes = GribBuilder::BuildGrib1(Grib1MessageSpec{
          .gds = Grib1GdsSpec{.data_representation_type = 1},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "unsupported data representation type (1)",
  });

  // 5. GDS Ni == 65535 && Nj == 65535.
  cases.push_back({
      .name = "grib1_both_missing.grb",
      .bytes = GribBuilder::BuildGrib1(Grib1MessageSpec{
          .gds = Grib1GdsSpec{.ni = 65535, .nj = 65535},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "MISSING value (65535) for both Ni and Nj",
  });

  // 6. GDS missing Ni without listOfNumbers (PVL == 255 and PVL != 255).
  cases.push_back({
      .name = "grib1_missing_ni_pvl255.grb",
      .bytes = GribBuilder::BuildGrib1(Grib1MessageSpec{
          .gds = Grib1GdsSpec{.ni = 65535, .nj = 2, .pvl_override = 255},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "without 'List of numbers'",
  });
  cases.push_back({
      .name = "grib1_missing_ni_pvl33_nolist.grb",
      .bytes = GribBuilder::BuildGrib1(Grib1MessageSpec{
          .gds =
              Grib1GdsSpec{.ni = 65535, .nj = 2, .coordinate_values = {1.0f}},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "without 'List of numbers'",
  });

  // 7. GDS listOfNumbers present when neither Ni nor Nj is 65535.
  cases.push_back({
      .name = "grib1_unexpected_list.grb",
      .bytes = GribBuilder::BuildGrib1(Grib1MessageSpec{
          .gds = Grib1GdsSpec{.ni = 2, .nj = 2, .list_of_numbers = {2, 2}},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "only be present if Ni or Nj have a MISSING value",
  });

  // 8. GDS listOfNumbers wrong length (Ni=65535, Nj=2, list size 1 -> 2 != 4).
  cases.push_back({
      .name = "grib1_bad_list_len.grb",
      .bytes = GribBuilder::BuildGrib1(Grib1MessageSpec{
          .gds = Grib1GdsSpec{.ni = 65535, .nj = 2, .list_of_numbers = {2}},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "invalid length (2) for 'list of numbers' in GDS",
  });

  // 9. Truncated GDS coordinateValues and listOfNumbers.
  {
    std::vector<uint8_t> b = GribBuilder::BuildGrib1(Grib1MessageSpec{
        .gds = Grib1GdsSpec{.coordinate_values = {1.0f, 2.0f}},
    });
    b.resize(8 + 28 + 32 + 2);
    cases.push_back({
        .name = "grib1_trunc_coords.grb",
        .bytes = std::move(b),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });

    std::vector<uint8_t> b2 = GribBuilder::BuildGrib1(Grib1MessageSpec{
        .gds = Grib1GdsSpec{.ni = 65535, .nj = 2, .list_of_numbers = {2, 2}},
    });
    b2.resize(8 + 28 + 32 + 1);
    cases.push_back({
        .name = "grib1_trunc_list.grb",
        .bytes = std::move(b2),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });
  }

  // 10. Unsupported predefined gridDefinition when GDS is absent.
  cases.push_back({
      .name = "grib1_bad_predef_grid.grb",
      .bytes = GribBuilder::BuildGrib1(Grib1MessageSpec{
          .grid_definition = 255,
          .gds = std::nullopt,
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "gridDefinition (255) not supported",
  });

  // 11. BMS errors: truncated header, predefined bitmap, too-small size,
  //     truncated body.
  {
    std::vector<uint8_t> b = GribBuilder::BuildGrib1(Grib1MessageSpec{
        .bms = Grib1BmsSpec{.bitmap = {true, true, true, true}},
    });
    b.resize(8 + 28 + 32 + 3);
    cases.push_back({
        .name = "grib1_trunc_bms_hdr.grb",
        .bytes = std::move(b),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });
  }
  cases.push_back({
      .name = "grib1_predef_bms.grb",
      .bytes = GribBuilder::BuildGrib1(Grib1MessageSpec{
          .bms = Grib1BmsSpec{.bitmap = {true, true, true, true},
                              .predefined_bitmap = 1},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "predefined bit map not supported",
  });
  cases.push_back({
      .name = "grib1_small_bms.grb",
      .bytes = GribBuilder::BuildGrib1(Grib1MessageSpec{
          .gds = Grib1GdsSpec{.ni = 5, .nj = 2},
          .bms = Grib1BmsSpec{.raw_bytes = {0xFF}},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "does not match expected size",
  });
  {
    std::vector<uint8_t> b = GribBuilder::BuildGrib1(Grib1MessageSpec{
        .bms = Grib1BmsSpec{.bitmap = {true, true, true, true}},
    });
    b.resize(8 + 28 + 32 + 6);
    cases.push_back({
        .name = "grib1_trunc_bms_body.grb",
        .bytes = std::move(b),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });
  }

  // 12. BDS errors both with and without BMS (covers `if (bitmask != NULL)
  // free`).
  for (const bool with_bms : {false, true}) {
    const std::optional<Grib1BmsSpec> bms =
        with_bms ? std::optional<Grib1BmsSpec>(
                       Grib1BmsSpec{.bitmap = {true, true, true, true}})
                 : std::nullopt;
    const size_t bms_bytes = with_bms ? 7 : 0;

    std::vector<uint8_t> trunc_bds = GribBuilder::BuildGrib1(Grib1MessageSpec{
        .bms = bms,
    });
    trunc_bds.resize(8 + 28 + 32 + bms_bytes + 5);
    cases.push_back({
        .name = with_bms ? "grib1_trunc_bds_bms.grb" : "grib1_trunc_bds.grb",
        .bytes = std::move(trunc_bds),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });
    cases.push_back({
        .name = with_bms ? "grib1_bds_sph_bms.grb" : "grib1_bds_sph.grb",
        .bytes = GribBuilder::BuildGrib1(Grib1MessageSpec{
            .bms = bms,
            .bds = Grib1BdsSpec{.flags = 0x80},
        }),
        .expected_errno = CODA_ERROR_PRODUCT,
        .expected_substr = "spherical harmonic",
    });
    cases.push_back({
        .name = with_bms ? "grib1_bds_cpx_bms.grb" : "grib1_bds_cpx.grb",
        .bytes = GribBuilder::BuildGrib1(Grib1MessageSpec{
            .bms = bms,
            .bds = Grib1BdsSpec{.flags = 0x40},
        }),
        .expected_errno = CODA_ERROR_PRODUCT,
        .expected_substr = "second order ('Complex') Packing",
    });
    cases.push_back({
        .name = with_bms ? "grib1_bds_ext_bms.grb" : "grib1_bds_ext.grb",
        .bytes = GribBuilder::BuildGrib1(Grib1MessageSpec{
            .bms = bms,
            .bds = Grib1BdsSpec{.flags = 0x10},
        }),
        .expected_errno = CODA_ERROR_PRODUCT,
        .expected_substr = "additional flags in BDS",
    });
    cases.push_back({
        .name = with_bms ? "grib1_bds_bpv64_bms.grb" : "grib1_bds_bpv64.grb",
        .bytes = GribBuilder::BuildGrib1(Grib1MessageSpec{
            .bms = bms,
            .bds = Grib1BdsSpec{.bits_per_value = 64},
        }),
        .expected_errno = CODA_ERROR_PRODUCT,
        .expected_substr = "bitsPerValue (64) too large",
    });
  }

  // 13. Truncated and invalid Section 5 ('7777').
  {
    std::vector<uint8_t> b = GribBuilder::BuildGrib1(Grib1MessageSpec{});
    b.resize(b.size() - 2);
    cases.push_back({
        .name = "grib1_trunc_7777.grb",
        .bytes = std::move(b),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });
  }
  cases.push_back({
      .name = "grib1_bad_7777.grb",
      .bytes = GribBuilder::BuildGrib1(Grib1MessageSpec{
          .end_marker = {'9', '9', '9', '9'},
      }),
      .expected_errno = CODA_ERROR_FILE_READ,
      .expected_substr = "invalid GRIB termination section",
  });

  for (const ErrorCase& tc : cases) {
    SCOPED_TRACE(tc.name);
    ASSERT_NO_FATAL_FAILURE(ExpectGribOpenFails(
        tc.name, tc.bytes, tc.expected_errno, tc.expected_substr));
  }
}

TEST(CodaGribTest, Grib2IdentificationLocalUseAndGridTemplates) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  GribBuilder builder;

  // Message 0: Full Identification metadata, extra ident bytes (>21), empty
  // Section 2 (len=5 -> skipped), non-empty Section 2 (len>5), and template 0.
  Grib2MessageSpec msg0;
  msg0.discipline = 2;
  msg0.centre = 98;
  msg0.sub_centre = 4;
  msg0.master_tables_version = 15;
  msg0.local_tables_version = 2;
  msg0.significance_of_reference_time = 1;
  msg0.year = 2026;
  msg0.month = 6;
  msg0.day = 15;
  msg0.hour = 12;
  msg0.minute = 30;
  msg0.second = 45;
  msg0.production_status = 3;
  msg0.type_of_processed_data = 2;
  msg0.ident_extra_bytes = {0x01, 0x02, 0x03};
  msg0.fields = {
      Grib2FieldSpec{
          .local_use = std::vector<uint8_t>{},  // 5-byte empty Section 2
          .grid =
              Grib2GridSpec{
                  .template_number = 0,
                  .shape_of_the_earth = 6,
                  .scale_factor_radius = 1,
                  .scaled_value_radius = 6371229,
                  .scale_factor_major_axis = 2,
                  .scaled_value_major_axis = 6378137,
                  .scale_factor_minor_axis = 3,
                  .scaled_value_minor_axis = 6356752,
                  .ni = 2,
                  .nj = 2,
                  .basic_angle = 1,
                  .subdivisions_of_basic_angle = 1000000,
                  .latitude_first = 1000000,
                  .longitude_first = 2000000,
                  .resolution_and_component_flags = 0x30,
                  .latitude_last = 3000000,
                  .longitude_last = 4000000,
                  .i_direction_increment = 500000,
                  .j_direction_increment_or_n = 750000,
                  .scanning_mode = 0x40,
                  .extra_template_bytes = {0xFE, 0xED},  // section_size > 72
              },
      },
      Grib2FieldSpec{
          .local_use = std::vector<uint8_t>{0xCA, 0xFE, 0xBA, 0xBE},
          .grid =
              Grib2GridSpec{
                  .template_number = 40,
                  .ni = 2,
                  .nj = 2,
                  .j_direction_increment_or_n = 128,
              },
      },
  };
  builder.AddGrib2(msg0);

  // Message 1: Quasi-regular grids with number_size = 1, 2, 4 and templates
  // 1, 2, 3, 41, 42, 43.
  Grib2MessageSpec msg1;
  msg1.fields = {
      Grib2FieldSpec{
          .grid =
              Grib2GridSpec{
                  .number_size_override = 1,
                  .template_number = 1,
                  .ni = 0xFFFFFFFFu,
                  .nj = 2,
                  .list_of_numbers = {2, 3},
              },
          .data_rep = Grib2DataRepSpec{.number_of_data_points = 5},
      },
      Grib2FieldSpec{
          .grid =
              Grib2GridSpec{
                  .number_size_override = 2,
                  .template_number = 41,
                  .ni = 2,
                  .nj = 0xFFFFFFFFu,
                  .list_of_numbers = {4, 1},
              },
          .data_rep = Grib2DataRepSpec{.number_of_data_points = 5},
      },
      Grib2FieldSpec{
          .grid =
              Grib2GridSpec{
                  .number_size_override = 4,
                  .template_number = 42,
                  .ni = 0xFFFFFFFFu,
                  .nj = 2,
                  .list_of_numbers = {2, 2},
              },
          .data_rep = Grib2DataRepSpec{.number_of_data_points = 4},
      },
      Grib2FieldSpec{
          .grid = Grib2GridSpec{.template_number = 2},
      },
      Grib2FieldSpec{
          .grid = Grib2GridSpec{.template_number = 3},
      },
      Grib2FieldSpec{
          .grid = Grib2GridSpec{.template_number = 43},
      },
  };
  builder.AddGrib2(msg1);

  TempFile file("grib2_ident_grid.grb2");
  ASSERT_TRUE(file.WriteBytes(builder.Build()));
  ScopedProduct product;
  {
    // TODO(schwehr): coda-grib.c:2342-2358 leaks `raw_data` after copying it
    // via coda_mem_raw_new() for GRIB2 Section 2 with section_size > 5.
    const absl::LeakCheckDisabler leak_disabler;
    ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));
  }

  // Verify union selection for GRIB2.
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(GotoPath(product.get(), "/[0]", &cursor));
  long union_idx = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_available_union_field_index(&cursor, &union_idx));
  EXPECT_EQ(1, union_idx);

  // Message 0 Identification fields.
  ExpectUint8(product.get(), "/[0]/grib2/editionNumber", 2);
  ExpectUint8(product.get(), "/[0]/grib2/discipline", 2);
  ExpectUint16(product.get(), "/[0]/grib2/centre", 98);
  ExpectUint16(product.get(), "/[0]/grib2/subCentre", 4);
  ExpectUint8(product.get(), "/[0]/grib2/masterTablesVersion", 15);
  ExpectUint8(product.get(), "/[0]/grib2/localTablesVersion", 2);
  ExpectUint8(product.get(), "/[0]/grib2/significanceOfReferenceTime", 1);
  ExpectUint16(product.get(), "/[0]/grib2/year", 2026);
  ExpectUint8(product.get(), "/[0]/grib2/month", 6);
  ExpectUint8(product.get(), "/[0]/grib2/day", 15);
  ExpectUint8(product.get(), "/[0]/grib2/hour", 12);
  ExpectUint8(product.get(), "/[0]/grib2/minute", 30);
  ExpectUint8(product.get(), "/[0]/grib2/second", 45);
  ExpectUint8(product.get(), "/[0]/grib2/productionStatusOfProcessedData", 3);
  ExpectUint8(product.get(), "/[0]/grib2/typeOfProcessedData", 2);

  // Local Use: only field 1 had section_size > 5.
  ExpectArrayCount(product.get(), "/[0]/grib2/local", 1);
  std::vector<uint8_t> local_bytes;
  ASSERT_NO_FATAL_FAILURE(
      ReadRawBytes(product.get(), "/[0]/grib2/local[0]", &local_bytes));
  EXPECT_THAT(local_bytes, ElementsAre(0xCA, 0xFE, 0xBA, 0xBE));

  // Grid 0 (lat/lon template 0) & Grid 1 (Gaussian template 40).
  ExpectInt32(product.get(), "/[0]/grib2/grid[0]/localRecordIndex", -1);
  ExpectUint8(product.get(), "/[0]/grib2/grid[0]/sourceOfGridDefinition", 0);
  ExpectUint32(product.get(), "/[0]/grib2/grid[0]/numberOfDataPoints", 4);
  ExpectUint8(product.get(), "/[0]/grib2/grid[0]/interpretationOfListOfNumbers",
              0);
  ExpectUint16(product.get(), "/[0]/grib2/grid[0]/gridDefinitionTemplateNumber",
               0);
  ExpectUint8(product.get(), "/[0]/grib2/grid[0]/shapeOfTheEarth", 6);
  ExpectUint8(product.get(),
              "/[0]/grib2/grid[0]/scaleFactorOfRadiusOfSphericalEarth", 1);
  ExpectUint32(product.get(),
               "/[0]/grib2/grid[0]/scaledValueOfRadiusOfSphericalEarth",
               6371229);
  ExpectUint8(product.get(), "/[0]/grib2/grid[0]/scaleFactorOfEarthMajorAxis",
              2);
  ExpectUint32(product.get(), "/[0]/grib2/grid[0]/scaledValueOfEarthMajorAxis",
               6378137);
  ExpectUint8(product.get(), "/[0]/grib2/grid[0]/scaleFactorOfEarthMinorAxis",
              3);
  ExpectUint32(product.get(), "/[0]/grib2/grid[0]/scaledValueOfEarthMinorAxis",
               6356752);
  ExpectUint32(product.get(), "/[0]/grib2/grid[0]/Ni", 2);
  ExpectUint32(product.get(), "/[0]/grib2/grid[0]/Nj", 2);
  ExpectUint32(product.get(),
               "/[0]/grib2/grid[0]/basicAngleOfTheInitialProductionDomain", 1);
  ExpectUint32(product.get(), "/[0]/grib2/grid[0]/subdivisionsOfBasicAngle",
               1000000);
  ExpectInt32(product.get(), "/[0]/grib2/grid[0]/latitudeOfFirstGridPoint",
              1000000);
  ExpectInt32(product.get(), "/[0]/grib2/grid[0]/longitudeOfFirstGridPoint",
              2000000);
  ExpectUint8(product.get(), "/[0]/grib2/grid[0]/resolutionAndComponentFlags",
              0x30);
  ExpectInt32(product.get(), "/[0]/grib2/grid[0]/latitudeOfLastGridPoint",
              3000000);
  ExpectInt32(product.get(), "/[0]/grib2/grid[0]/longitudeOfLastGridPoint",
              4000000);
  ExpectUint32(product.get(), "/[0]/grib2/grid[0]/iDirectionIncrement", 500000);
  ExpectUint32(product.get(), "/[0]/grib2/grid[0]/jDirectionIncrement", 750000);
  ExpectUint8(product.get(), "/[0]/grib2/grid[0]/scanningMode", 0x40);
  ExpectFieldAvailable(product.get(), "/[0]/grib2/grid[0]", "N", false);

  ExpectInt32(product.get(), "/[0]/grib2/grid[1]/localRecordIndex", 0);
  ExpectUint32(product.get(), "/[0]/grib2/grid[1]/N", 128);
  ExpectFieldAvailable(product.get(), "/[0]/grib2/grid[1]",
                       "jDirectionIncrement", false);

  // Message 1: Quasi-regular listOfNumbers (1-, 2-, 4-byte) & remaining
  // templates.
  std::vector<uint32_t> list_sz1;
  ASSERT_NO_FATAL_FAILURE(ReadUint32Array(
      product.get(), "/[1]/grib2/grid[0]/listOfNumbers", &list_sz1));
  EXPECT_THAT(list_sz1, ElementsAre(2, 3));

  std::vector<uint32_t> list_sz2;
  ASSERT_NO_FATAL_FAILURE(ReadUint32Array(
      product.get(), "/[1]/grib2/grid[1]/listOfNumbers", &list_sz2));
  EXPECT_THAT(list_sz2, ElementsAre(4, 1));

  ExpectArrayCount(product.get(), "/[1]/grib2/grid[2]/listOfNumbers", 2);
  ExpectFieldAvailable(product.get(), "/[1]/grib2/grid[3]",
                       "jDirectionIncrement", true);
  ExpectFieldAvailable(product.get(), "/[1]/grib2/grid[4]",
                       "jDirectionIncrement", true);
  ExpectFieldAvailable(product.get(), "/[1]/grib2/grid[5]", "N", true);
}

TEST(CodaGribTest, Grib2ProductDataRepBitmapAndMultiFieldReuse) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  GribBuilder builder;

  // Message 0: Multi-field GRIB2 exercising templates 0 & 40, fixed-surface
  // scaling (-/+/255), IEEE float32 coordinateValues, DataRep 0 & 1, and
  // Section 6 bitmap 0 (inline), 254 (reuse), 255 (none), and omitted
  // Section 6.
  Grib2MessageSpec msg0;
  msg0.fields = {
      // Field 0: Template 0, inline bitmap (0), IEEE raw coordinateValues,
      // DataRep 0.
      Grib2FieldSpec{
          .product =
              Grib2ProductSpec{
                  .template_number = 0,
                  .parameter_category = 1,
                  .parameter_number = 8,
                  .type_of_generating_process = 2,
                  .background_process = 3,
                  .generating_process_id = 99,
                  .hours_after_data_cutoff = 1,
                  .minutes_after_data_cutoff = 15,
                  .indicator_of_unit_of_time_range = 1,
                  .forecast_time = 24,
                  .type_of_first_fixed_surface = 100,
                  .scale_factor_first_fixed_surface = -2,
                  .scaled_value_first_fixed_surface = 5,  // 500.0
                  .type_of_second_fixed_surface = 100,
                  .scale_factor_second_fixed_surface = 2,
                  .scaled_value_second_fixed_surface = 250,  // 2.5
                  .coordinate_values = {1.5f, -2.25f, 3.75f},
              },
          .data_rep =
              Grib2DataRepSpec{
                  .number_of_data_points = 4,
                  .template_number = 0,
                  .reference_value = 10.0f,
                  .binary_scale_factor = -1,
                  .decimal_scale_factor = 1,
                  .bits_per_value = 8,
              },
          .bitmap = Grib2BitmapSpec{.bitmap = {true, false, true, true}},
          .data = Grib2DataSpec{.packed_ints = {0, 2, 6}},
      },
      // Field 1: Section 3 omitted (reuses grid 0), template 40 with
      // constituentType & NaN surfaces, DataRep 1 with extra bytes, bitmap 254.
      Grib2FieldSpec{
          .grid = std::nullopt,
          .product =
              Grib2ProductSpec{
                  .template_number = 40,
                  .parameter_category = 20,
                  .parameter_number = 2,
                  .constituent_type = 40010,
                  .type_of_first_fixed_surface = 255,
                  .type_of_second_fixed_surface = 255,
              },
          .data_rep =
              Grib2DataRepSpec{
                  .number_of_data_points = 4,
                  .template_number = 1,
                  .reference_value = 5.0f,
                  .binary_scale_factor = 1,
                  .decimal_scale_factor = -1,
                  .bits_per_value = 8,
                  .extra_template_bytes = {0, 0, 0, 0},
              },
          .bitmap = Grib2BitmapSpec{.indicator_override = 254},
          .data = Grib2DataSpec{.packed_ints = {0, 1, 2}},
      },
      // Field 2: Template 40 with non-255 surfaces (+/- scale), bitmap 255.
      Grib2FieldSpec{
          .product =
              Grib2ProductSpec{
                  .template_number = 40,
                  .constituent_type = 5,
                  .type_of_first_fixed_surface = 105,
                  .scale_factor_first_fixed_surface = -1,
                  .scaled_value_first_fixed_surface = 2,  // 20.0
                  .type_of_second_fixed_surface = 105,
                  .scale_factor_second_fixed_surface = 1,
                  .scaled_value_second_fixed_surface = 50,
              },
          .data_rep =
              Grib2DataRepSpec{
                  .number_of_data_points = 4,
                  .template_number = 0,
                  .reference_value = 7.25f,
                  .bits_per_value = 0,
              },
          .bitmap = Grib2BitmapSpec{.indicator_override = 255},
      },
      // Field 3: Template 40 with flipped surface signs, Section 6 omitted.
      Grib2FieldSpec{
          .grid = std::nullopt,
          .product =
              Grib2ProductSpec{
                  .template_number = 40,
                  .type_of_first_fixed_surface = 105,
                  .scale_factor_first_fixed_surface = 1,
                  .scaled_value_first_fixed_surface = 30,  // 3.0
                  .type_of_second_fixed_surface = 105,
                  .scale_factor_second_fixed_surface = -1,
                  .scaled_value_second_fixed_surface = 4,
              },
          .data_rep =
              Grib2DataRepSpec{
                  .number_of_data_points = 4,
                  .bits_per_value = 8,
              },
          .bitmap = std::nullopt,
          .data = Grib2DataSpec{.packed_ints = {1, 2, 3, 4}},
      },
      // Field 4: Template 15 with typeOfFirstFixedSurface = 255 -> NaN.
      Grib2FieldSpec{
          .grid = std::nullopt,
          .product =
              Grib2ProductSpec{
                  .template_number = 15,
                  .type_of_first_fixed_surface = 255,
                  .type_of_second_fixed_surface = 255,
              },
          .data_rep =
              Grib2DataRepSpec{.number_of_data_points = 4, .bits_per_value = 0},
          .bitmap = std::nullopt,
      },
  };
  builder.AddGrib2(msg0);

  // Message 1: Cover remaining supported Product templates (1..6, 51).
  Grib2MessageSpec msg1;
  for (const uint16_t pdt : {uint16_t{1}, uint16_t{2}, uint16_t{3}, uint16_t{4},
                             uint16_t{5}, uint16_t{6}, uint16_t{51}}) {
    msg1.fields.push_back(Grib2FieldSpec{
        .product = Grib2ProductSpec{.template_number = pdt},
        .data_rep =
            Grib2DataRepSpec{.number_of_data_points = 4, .bits_per_value = 0},
    });
  }
  builder.AddGrib2(msg1);

  TempFile file("grib2_product_data.grb2");
  ASSERT_TRUE(file.WriteBytes(builder.Build()));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

  // Field 0 checks.
  ExpectUint32(product.get(), "/[0]/grib2/data[0]/gridRecordIndex", 0);
  ExpectUint8(product.get(), "/[0]/grib2/data[0]/parameterCategory", 1);
  ExpectUint8(product.get(), "/[0]/grib2/data[0]/parameterNumber", 8);
  ExpectFieldAvailable(product.get(), "/[0]/grib2/data[0]", "constituentType",
                       false);
  ExpectUint8(product.get(), "/[0]/grib2/data[0]/typeOfGeneratingProcess", 2);
  ExpectUint8(product.get(), "/[0]/grib2/data[0]/backgroundProcess", 3);
  ExpectUint8(product.get(), "/[0]/grib2/data[0]/generatingProcessIdentifier",
              99);
  ExpectUint16(product.get(), "/[0]/grib2/data[0]/hoursAfterDataCutoff", 1);
  ExpectUint8(product.get(), "/[0]/grib2/data[0]/minutesAfterDataCutoff", 15);
  ExpectUint8(product.get(), "/[0]/grib2/data[0]/indicatorOfUnitOfTimeRange",
              1);
  ExpectUint32(product.get(), "/[0]/grib2/data[0]/forecastTime", 24);
  ExpectUint8(product.get(), "/[0]/grib2/data[0]/typeOfFirstFixedSurface", 100);
  ExpectDouble(product.get(), "/[0]/grib2/data[0]/firstFixedSurface", 500.0);
  ExpectUint8(product.get(), "/[0]/grib2/data[0]/typeOfSecondFixedSurface",
              100);
  ExpectDouble(product.get(), "/[0]/grib2/data[0]/secondFixedSurface", 2.5);
  ExpectFieldAvailable(product.get(), "/[0]/grib2/data[0]", "coordinateValues",
                       true);
  std::vector<float> coords0;
  ASSERT_NO_FATAL_FAILURE(ReadFloatArray(
      product.get(), "/[0]/grib2/data[0]/coordinateValues", &coords0));
  EXPECT_THAT(coords0, ElementsAre(1.5f, -2.25f, 3.75f));

  ExpectUint8(product.get(), "/[0]/grib2/data[0]/bitsPerValue", 8);
  ExpectInt16(product.get(), "/[0]/grib2/data[0]/decimalScaleFactor", 1);
  ExpectInt16(product.get(), "/[0]/grib2/data[0]/binaryScaleFactor", -1);
  ExpectFloat(product.get(), "/[0]/grib2/data[0]/referenceValue", 10.0f);
  std::vector<float> vals0;
  ASSERT_NO_FATAL_FAILURE(
      ReadFloatArray(product.get(), "/[0]/grib2/data[0]/values", &vals0));
  ASSERT_EQ(4u, vals0.size());
  EXPECT_FLOAT_EQ(1.0f, vals0[0]);
  EXPECT_TRUE(std::isnan(vals0[1]));
  EXPECT_FLOAT_EQ(1.1f, vals0[2]);
  EXPECT_FLOAT_EQ(1.3f, vals0[3]);

  // Field 1: reused grid 0, template 40 constituentType, reused bitmap 254.
  ExpectUint32(product.get(), "/[0]/grib2/data[1]/gridRecordIndex", 0);
  ExpectFieldAvailable(product.get(), "/[0]/grib2/data[1]", "constituentType",
                       true);
  ExpectUint16(product.get(), "/[0]/grib2/data[1]/constituentType", 40010);
  ExpectDoubleNaN(product.get(), "/[0]/grib2/data[1]/firstFixedSurface");
  ExpectDoubleNaN(product.get(), "/[0]/grib2/data[1]/secondFixedSurface");
  ExpectFieldAvailable(product.get(), "/[0]/grib2/data[1]", "coordinateValues",
                       false);
  std::vector<float> vals1;
  ASSERT_NO_FATAL_FAILURE(
      ReadFloatArray(product.get(), "/[0]/grib2/data[1]/values", &vals1));
  ASSERT_EQ(4u, vals1.size());
  EXPECT_FLOAT_EQ(50.0f, vals1[0]);
  EXPECT_TRUE(std::isnan(vals1[1]));
  EXPECT_FLOAT_EQ(70.0f, vals1[2]);
  EXPECT_FLOAT_EQ(90.0f, vals1[3]);

  // Field 2: new grid 1, bitmap 255 (no mask), bitsPerValue == 0.
  ExpectUint32(product.get(), "/[0]/grib2/data[2]/gridRecordIndex", 1);
  ExpectDouble(product.get(), "/[0]/grib2/data[2]/firstFixedSurface", 20.0);
  std::vector<float> vals2;
  ASSERT_NO_FATAL_FAILURE(
      ReadFloatArray(product.get(), "/[0]/grib2/data[2]/values", &vals2));
  EXPECT_THAT(vals2, ElementsAre(7.25f, 7.25f, 7.25f, 7.25f));

  // Field 3: Section 6 omitted.
  ExpectDouble(product.get(), "/[0]/grib2/data[3]/firstFixedSurface", 3.0);
  std::vector<float> vals3;
  ASSERT_NO_FATAL_FAILURE(
      ReadFloatArray(product.get(), "/[0]/grib2/data[3]/values", &vals3));
  EXPECT_THAT(vals3, ElementsAre(1.0f, 2.0f, 3.0f, 4.0f));

  // Field 4: Template 15 with NaN surfaces.
  ExpectDoubleNaN(product.get(), "/[0]/grib2/data[4]/firstFixedSurface");
  ExpectDoubleNaN(product.get(), "/[0]/grib2/data[4]/secondFixedSurface");

  // Message 1: 7 fields for templates 1..6, 51.
  ExpectArrayCount(product.get(), "/[1]/grib2/data", 7);
}

TEST(CodaGribTest, Grib2MalformedAndSectionOrderErrorPaths) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  struct ErrorCase {
    std::string_view name;
    std::vector<uint8_t> bytes;
    int expected_errno;
    std::string_view expected_substr;
  };
  std::vector<ErrorCase> cases;

  // 1. Truncated Section 0 64-bit length (only 12 bytes).
  cases.push_back({
      .name = "grib2_trunc_sec0.grb2",
      .bytes = {'G', 'R', 'I', 'B', 0, 0, 0, 2, 0, 0, 0, 0},
      .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
      .expected_substr = "beyond the end of the file",
  });

  // 2. Truncated Section 1 (<21 bytes) and wrong Section 1 number.
  cases.push_back({
      .name = "grib2_trunc_sec1.grb2",
      .bytes = {'G', 'R', 'I', 'B', 0, 0, 0, 2,  0, 0, 0, 0,
                0,   0,   0,   40,  0, 0, 0, 21, 1, 0, 98},
      .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
      .expected_substr = "beyond the end of the file",
  });
  cases.push_back({
      .name = "grib2_bad_sec1_num.grb2",
      .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
          .section1_number_override = 2,
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "for Identification Section",
  });

  // 3. Truncated before next section / section number / invalid section number.
  {
    std::vector<uint8_t> b = GribBuilder::BuildGrib2(Grib2MessageSpec{
        .omit_data_sections = true,
    });
    b.resize(b.size() - 2);
    cases.push_back({
        .name = "grib2_trunc_end.grb2",
        .bytes = std::move(b),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });

    std::vector<uint8_t> b2 = GribBuilder::BuildGrib2(Grib2MessageSpec{
        .raw_sections = {{.section_number = 9}},
    });
    b2.resize(16 + 21 + 4);
    cases.push_back({
        .name = "grib2_trunc_sec_num.grb2",
        .bytes = std::move(b2),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });

    std::vector<uint8_t> b3 = GribBuilder::BuildGrib2(Grib2MessageSpec{});
    b3.resize(b3.size() - 2);
    cases.push_back({
        .name = "grib2_trunc_7777.grb2",
        .bytes = std::move(b3),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });
  }
  cases.push_back({
      .name = "grib2_invalid_sec_num.grb2",
      .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
          .raw_sections = {{.section_number = 9}},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "invalid Section Number",
  });

  // 4. Unexpected section orderings (Sec 2 after 3, 3 after 3, 4 after 1,
  //    5 after 3, 6 after 4, 7 after 4).
  {
    const std::vector<uint8_t> dummy_grid_body(67, 0);
    cases.push_back({
        .name = "grib2_sec2_after_sec3.grb2",
        .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
            .raw_sections =
                {
                    {.section_number = 3, .body = dummy_grid_body},
                    {.section_number = 2, .body = {1, 2}},
                },
        }),
        .expected_errno = CODA_ERROR_PRODUCT,
        .expected_substr = "after 3)",
    });
    cases.push_back({
        .name = "grib2_sec3_after_sec3.grb2",
        .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
            .raw_sections =
                {
                    {.section_number = 3, .body = dummy_grid_body},
                    {.section_number = 3, .body = dummy_grid_body},
                },
        }),
        .expected_errno = CODA_ERROR_PRODUCT,
        .expected_substr = "after 3)",
    });
    cases.push_back({
        .name = "grib2_sec4_after_sec1.grb2",
        .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
            .raw_sections = {{.section_number = 4,
                              .body = std::vector<uint8_t>(29, 0)}},
        }),
        .expected_errno = CODA_ERROR_PRODUCT,
        .expected_substr = "after 1)",
    });
    cases.push_back({
        .name = "grib2_sec5_after_sec3.grb2",
        .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
            .raw_sections =
                {
                    {.section_number = 3, .body = dummy_grid_body},
                    {.section_number = 5, .body = std::vector<uint8_t>(16, 0)},
                },
        }),
        .expected_errno = CODA_ERROR_PRODUCT,
        .expected_substr = "after 3)",
    });
    cases.push_back({
        .name = "grib2_sec6_after_sec4.grb2",
        .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
            .raw_sections =
                {
                    {.section_number = 3, .body = dummy_grid_body},
                    {.section_number = 4, .body = std::vector<uint8_t>(29, 0)},
                    {.section_number = 6, .body = {255}},
                },
        }),
        .expected_errno = CODA_ERROR_PRODUCT,
        .expected_substr = "after 4)",
    });
    cases.push_back({
        .name = "grib2_sec7_after_sec4.grb2",
        .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
            .raw_sections =
                {
                    {.section_number = 3, .body = dummy_grid_body},
                    {.section_number = 4, .body = std::vector<uint8_t>(29, 0)},
                    {.section_number = 7, .body = {0, 0}},
                },
        }),
        .expected_errno = CODA_ERROR_PRODUCT,
        .expected_substr = "after 4)",
    });
  }

  // 5. Truncated Section 2 body.
  {
    std::vector<uint8_t> b = GribBuilder::BuildGrib2(Grib2MessageSpec{
        .local_use = std::vector<uint8_t>{1, 2, 3, 4, 5, 6},
    });
    b.resize(16 + 21 + 5 + 3);
    cases.push_back({
        .name = "grib2_trunc_sec2_body.grb2",
        .bytes = std::move(b),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });
  }

  // 6. Section 3 errors: truncated header/body, unsupported source/template,
  //    quasi-regular errors, unsupported number_size=3, truncated list.
  {
    std::vector<uint8_t> b = GribBuilder::BuildGrib2(Grib2MessageSpec{});
    b.resize(16 + 21 + 5 + 4);
    cases.push_back({
        .name = "grib2_trunc_sec3_hdr.grb2",
        .bytes = std::move(b),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });

    std::vector<uint8_t> b2 = GribBuilder::BuildGrib2(Grib2MessageSpec{});
    b2.resize(16 + 21 + 5 + 9 + 20);
    cases.push_back({
        .name = "grib2_trunc_sec3_body.grb2",
        .bytes = std::move(b2),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });
  }
  cases.push_back({
      .name = "grib2_bad_grid_source.grb2",
      .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
          .grid = Grib2GridSpec{.source_of_grid_definition = 1},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "unsupported grid source/template (1/0)",
  });
  cases.push_back({
      .name = "grib2_bad_grid_template.grb2",
      .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
          .grid = Grib2GridSpec{.template_number = 10},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "unsupported grid source/template (0/10)",
  });
  cases.push_back({
      .name = "grib2_missing_ni_no_list.grb2",
      .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
          .grid =
              Grib2GridSpec{
                  .interpretation_override = 0, .ni = 0xFFFFFFFFu, .nj = 2},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "without 'List of numbers'",
  });
  cases.push_back({
      .name = "grib2_list_without_missing.grb2",
      .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
          .grid = Grib2GridSpec{.ni = 2, .nj = 2, .list_of_numbers = {2, 2}},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "only be present if Ni or Nj have a MISSING value",
  });
  cases.push_back({
      .name = "grib2_bad_list_len.grb2",
      .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
          .grid = Grib2GridSpec{.number_size_override = 2,
                                .ni = 0xFFFFFFFFu,
                                .nj = 2,
                                .list_of_numbers = {2}},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "invalid length (2) for 'list of numbers' in GDS",
  });
  {
    // Emit 6 bytes in list_of_numbers with Nj=2 (so 6 == 3*2) and patch
    // Section 3 `number_size` byte at offset 16 + 21 + 10 = 47 to 3.
    std::vector<uint8_t> b = GribBuilder::BuildGrib2(Grib2MessageSpec{
        .grid = Grib2GridSpec{.number_size_override = 1,
                              .ni = 0xFFFFFFFFu,
                              .nj = 2,
                              .list_of_numbers = {1, 1, 1, 1, 1, 1}},
    });
    b[47] = 3;
    cases.push_back({
        .name = "grib2_bad_list_octet_size.grb2",
        .bytes = std::move(b),
        .expected_errno = CODA_ERROR_PRODUCT,
        .expected_substr = "unsupported octect size (3)",
    });

    std::vector<uint8_t> b2 = GribBuilder::BuildGrib2(Grib2MessageSpec{
        .grid = Grib2GridSpec{.number_size_override = 2,
                              .ni = 0xFFFFFFFFu,
                              .nj = 2,
                              .list_of_numbers = {2, 2}},
    });
    b2.resize(16 + 21 + 72 + 1);
    cases.push_back({
        .name = "grib2_trunc_list.grb2",
        .bytes = std::move(b2),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });
  }

  // 7. Section 4 errors: truncated header, truncated template 0/40,
  // unsupported 8.
  {
    const size_t sec4_off = 16 + 21 + 72;
    std::vector<uint8_t> b = GribBuilder::BuildGrib2(Grib2MessageSpec{});
    b.resize(sec4_off + 5 + 2);
    cases.push_back({
        .name = "grib2_trunc_sec4_hdr.grb2",
        .bytes = std::move(b),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });

    std::vector<uint8_t> b2 = GribBuilder::BuildGrib2(Grib2MessageSpec{});
    b2.resize(sec4_off + 5 + 4 + 10);
    cases.push_back({
        .name = "grib2_trunc_sec4_t0.grb2",
        .bytes = std::move(b2),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });

    std::vector<uint8_t> b3 = GribBuilder::BuildGrib2(Grib2MessageSpec{
        .product = Grib2ProductSpec{.template_number = 40},
    });
    b3.resize(sec4_off + 5 + 4 + 10);
    cases.push_back({
        .name = "grib2_trunc_sec4_t40.grb2",
        .bytes = std::move(b3),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });
  }
  cases.push_back({
      .name = "grib2_bad_prod_template.grb2",
      .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
          .product = Grib2ProductSpec{.template_number = 8},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "unsupported Product Definition Template (8)",
  });

  // 8. Section 5 errors: truncated header / ref / scale+bits, bitsPerValue>63,
  //    unsupported template 4.
  {
    const size_t sec5_off = 16 + 21 + 72 + 34;
    std::vector<uint8_t> b = GribBuilder::BuildGrib2(Grib2MessageSpec{});
    b.resize(sec5_off + 5 + 3);
    cases.push_back({
        .name = "grib2_trunc_sec5_hdr.grb2",
        .bytes = std::move(b),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });

    std::vector<uint8_t> b2 = GribBuilder::BuildGrib2(Grib2MessageSpec{});
    b2.resize(sec5_off + 5 + 6 + 2);
    cases.push_back({
        .name = "grib2_trunc_sec5_ref.grb2",
        .bytes = std::move(b2),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });

    std::vector<uint8_t> b3 = GribBuilder::BuildGrib2(Grib2MessageSpec{});
    b3.resize(sec5_off + 5 + 6 + 4 + 2);
    cases.push_back({
        .name = "grib2_trunc_sec5_scale.grb2",
        .bytes = std::move(b3),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });
  }
  cases.push_back({
      .name = "grib2_bad_bpv64.grb2",
      .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
          .data_rep = Grib2DataRepSpec{.bits_per_value = 64},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "bitsPerValue (64) too large",
  });
  cases.push_back({
      .name = "grib2_bad_datarep_template.grb2",
      .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
          .data_rep = Grib2DataRepSpec{.template_number = 4},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "unsupported Data Representation Template (4)",
  });

  // 9. Section 6 errors: truncated indicator, 254 without prior bitmap,
  //    unsupported indicator 1.
  {
    const size_t sec6_off = 16 + 21 + 72 + 34 + 21;
    std::vector<uint8_t> b = GribBuilder::BuildGrib2(Grib2MessageSpec{});
    b.resize(sec6_off + 5);
    cases.push_back({
        .name = "grib2_trunc_sec6.grb2",
        .bytes = std::move(b),
        .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
        .expected_substr = "beyond the end of the file",
    });
  }
  cases.push_back({
      .name = "grib2_bms_254_no_prev.grb2",
      .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
          .bitmap = Grib2BitmapSpec{.indicator_override = 254},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "no previously defined Bit Map found",
  });
  cases.push_back({
      .name = "grib2_bms_predef_1.grb2",
      .bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
          .bitmap = Grib2BitmapSpec{.indicator_override = 1},
      }),
      .expected_errno = CODA_ERROR_PRODUCT,
      .expected_substr = "pre-defined Bit Maps not supported",
  });

  for (const ErrorCase& tc : cases) {
    SCOPED_TRACE(tc.name);
    ASSERT_NO_FATAL_FAILURE(ExpectGribOpenFails(
        tc.name, tc.bytes, tc.expected_errno, tc.expected_substr));
  }
}

TEST(CodaGribTest, MultiMessageMixedEditionsAndFillerBytes) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  // 1. coda_open() on GRIB1 + inter-message filler + GRIB2 + trailing filler
  //    without 'G'.
  {
    GribBuilder builder;
    builder.AddGrib1(Grib1MessageSpec{.centre = 77});
    builder.AddFiller("INTER_PAD_BYTES");
    builder.AddGrib2(Grib2MessageSpec{.centre = 88});
    builder.AddFiller("TAIL_PAD_BYTES");

    TempFile file("grib_mixed_filler.grb");
    ASSERT_TRUE(file.WriteBytes(builder.Build()));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

    ExpectArrayCount(product.get(), "/", 2);
    ExpectUint8(product.get(), "/[0]/grib1/editionNumber", 1);
    ExpectUint8(product.get(), "/[0]/grib1/centre", 77);
    ExpectUint8(product.get(), "/[1]/grib2/editionNumber", 2);
    ExpectUint16(product.get(), "/[1]/grib2/centre", 88);
    EXPECT_EQ(CODA_SUCCESS, coda_close(product.release()));
  }

  // 2. Direct coda_grib_reopen() with leading filler before first message,
  //    inter-message filler, and trailing filler (all strictly without 'G').
  {
    GribBuilder builder;
    builder.AddFiller("HEAD_PAD_BYTES_1234");
    builder.AddGrib2(Grib2MessageSpec{.centre = 55});
    builder.AddFiller("MID_PAD_BYTES");
    builder.AddGrib1(Grib1MessageSpec{.centre = 66});
    builder.AddFiller("TAIL_PAD_BYTES");

    const std::vector<uint8_t> bytes = builder.Build();
    TempFile file("grib_direct_leading_filler.grb");
    ASSERT_TRUE(file.WriteBytes(bytes));

    coda_product* raw = nullptr;
    ASSERT_EQ(
        CODA_SUCCESS,
        coda_bin_open(file.c_str(), static_cast<int64_t>(bytes.size()), &raw));
    ASSERT_EQ(0, coda_grib_reopen(&raw));
    ScopedProduct product(raw);

    EXPECT_EQ(coda_format_grib, product.get()->format);
    ExpectArrayCount(product.get(), "/", 2);
    ExpectUint16(product.get(), "/[0]/grib2/centre", 55);
    ExpectUint8(product.get(), "/[1]/grib1/centre", 66);
    EXPECT_EQ(CODA_SUCCESS, coda_close(product.release()));
  }

  // 3. Direct coda_grib_reopen() on pure filler bytes without 'G' yields 0
  //    messages.
  {
    constexpr std::string_view kOnlyFiller = "ONLY_PAD_BYTES_NO_HEADER";
    const std::vector<uint8_t> bytes(kOnlyFiller.begin(), kOnlyFiller.end());
    TempFile file("grib_only_filler.grb");
    ASSERT_TRUE(file.WriteBytes(bytes));

    coda_product* raw = nullptr;
    ASSERT_EQ(
        CODA_SUCCESS,
        coda_bin_open(file.c_str(), static_cast<int64_t>(bytes.size()), &raw));
    ASSERT_EQ(0, coda_grib_reopen(&raw));
    ScopedProduct product(raw);
    ExpectArrayCount(product.get(), "/", 0);
  }

  // 4. Invalid indicator after 'G' in filler and truncated Section 0 after 'G'.
  {
    GribBuilder bad_ind;
    bad_ind.AddGrib1(Grib1MessageSpec{});
    bad_ind.AddFiller("GXYZ_NOT_GRIB");
    ASSERT_NO_FATAL_FAILURE(ExpectGribOpenFails(
        "grib_bad_indicator_msg1.grb", bad_ind.Build(), CODA_ERROR_PRODUCT,
        "invalid indicator for message 1"));

    GribBuilder trunc_ind;
    trunc_ind.AddGrib1(Grib1MessageSpec{});
    trunc_ind.AddFiller("GR_");
    ASSERT_NO_FATAL_FAILURE(ExpectGribOpenFails(
        "grib_trunc_indicator_msg1.grb", trunc_ind.Build(),
        CODA_ERROR_OUT_OF_BOUNDS_READ, "beyond the end of the file"));
  }

  // 5. Direct coda_grib_reopen() error paths at message 0 (bad indicator and
  //    unsupported edition).
  {
    const std::vector<uint8_t> bad_magic = {'G', 'X', 'Y', 'Z', 0, 0, 12, 1};
    ASSERT_NO_FATAL_FAILURE(ExpectGribReopenFails(
        "grib_direct_bad_magic.grb", bad_magic, CODA_ERROR_PRODUCT,
        "invalid indicator for message 0"));

    const std::vector<uint8_t> bad_ed = {'G', 'R', 'I', 'B', 0, 0, 12, 3};
    ASSERT_NO_FATAL_FAILURE(ExpectGribReopenFails(
        "grib_direct_bad_ed.grb", bad_ed, CODA_ERROR_UNSUPPORTED_PRODUCT,
        "unsupported GRIB format version (3) for message 0"));
  }
}

// TODO(schwehr): Reach OOM allocation-failure branches in coda-grib.c (lines
// 225-230, 1670-1675, 2072-2077, 2343-2348, 3025-3030, 3104-3110, 3124-3130)
// and WORDS_BIGENDIAN swap guards if fault injection or big-endian targets are
// added.

}  // namespace
}  // namespace stcorp_coda
