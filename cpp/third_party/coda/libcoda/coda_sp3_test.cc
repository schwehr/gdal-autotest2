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
// Tests CODA SP3 (Standard Product 3) ephemeris recognition, header/record
// ingestion, cursor reading, and error handling (libcoda/coda-sp3.c).
//
// google3 build caveats:
//   - Built for little-endian Linux (`#ifdef WORDS_BIGENDIAN` in sp3_init()
//     is compiled out).
//   - Internal malloc/strdup OOM branches and stdio `ferror()` branches
//     (`linelength < 0`) in read_header()/read_records() are unreachable
//     without fault injection.
//   - Auto-detection in libcoda/coda-product.c:299 recognizes SP3 versions
//     'a', 'b', and 'c' only; SP3d ('#d') is rejected by coda_open() as
//     unrecognized binary, though coda_sp3_reopen() can ingest it directly.
//   - Upstream coda-sp3.c:1129-1134 leaks `sat_accuracy` array when a
//     satellite accuracy integer fails to parse; that negative test wraps the
//     call in a narrow absl::LeakCheckDisabler.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-sp3.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-sp3.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-product.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-bin.h"
#include "third_party/stcorp_coda/libcoda/coda-sp3.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

// Isolates CODA environment variables, definition path, coda_option_use_mmap,
// and thread-local coda_errno across every test.
class ScopedSp3TestEnv {
 public:
  ScopedSp3TestEnv()
      : env_def_("CODA_DEFINITION"),
        env_leap_("CODA_LEAP_SECOND_TABLE"),
        saved_use_mmap_(coda_get_option_use_mmap()) {
    env_def_.Unset();
    env_leap_.Unset();
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  ~ScopedSp3TestEnv() {
    coda_set_option_use_mmap(saved_use_mmap_);
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  ScopedSp3TestEnv(const ScopedSp3TestEnv&) = delete;
  ScopedSp3TestEnv& operator=(const ScopedSp3TestEnv&) = delete;

 private:
  ScopedEnvVar env_def_;
  ScopedEnvVar env_leap_;
  int saved_use_mmap_;
};

struct Sp3HeaderConfig {
  char version = 'c';
  char pos_vel = 'P';
  std::string datetime_start = "2024 01 15 00 00  0.00000000";
  int num_epochs = 1;
  std::string data_used = "ORBIT";
  std::string coordinate_sys = "IGS20";
  std::string orbit_type = "FIT";
  std::string agency = "IGS ";
  int gps_week = 2297;
  double sec_of_week = 86400.0;
  double epoch_interval = 900.0;
  int mjd_start = 60324;
  double frac_day = 0.0;
  int num_satellites = 2;
  std::vector<std::string> sat_ids = {"G01", "G02"};
  std::vector<int> sat_accuracies = {5, 7};
  std::string file_type = "M ";
  std::string time_system = "GPS";
  double base_pos_vel = 1.25;
  double base_clk_rate = 1.025;
};

// Formats fixed-width 3-char strings, padding or truncating to `width`.
std::string FixedWidth(std::string_view sv, size_t width) {
  std::string out(width, ' ');
  const size_t n = sv.size() < width ? sv.size() : width;
  out.replace(0, n, sv.data(), n);
  return out;
}

// Builds the standard 22-line SP3 header (lines 1..22).
std::vector<std::string> BuildSp3HeaderLines(const Sp3HeaderConfig& cfg) {
  std::vector<std::string> lines;
  lines.reserve(22);

  // Line 1: version, P/V, start epoch (28), num_epochs (7), metadata.
  lines.push_back(absl::StrFormat(
      "#%c%c%s %7d %s %s %s %s", cfg.version, cfg.pos_vel,
      FixedWidth(cfg.datetime_start, 28), cfg.num_epochs,
      FixedWidth(cfg.data_used, 5), FixedWidth(cfg.coordinate_sys, 5),
      FixedWidth(cfg.orbit_type, 3), FixedWidth(cfg.agency, 4)));

  // Line 2: GPS week (4), sec of week (15), interval (14), MJD (5), frac (15).
  lines.push_back(absl::StrFormat(
      "## %4d %15.8f %14.8f %5d %15.13f", cfg.gps_week, cfg.sec_of_week,
      cfg.epoch_interval, cfg.mjd_start, cfg.frac_day));

  // Lines 3..7: '+' satellite IDs (up to 5 lines x 17 satellites).
  for (int line_idx = 0; line_idx < 5; ++line_idx) {
    std::string line = (line_idx == 0)
                           ? absl::StrFormat("+   %2d   ", cfg.num_satellites)
                           : std::string("+        ");
    for (int col = 0; col < 17; ++col) {
      const size_t idx = static_cast<size_t>(line_idx * 17 + col);
      if (idx < cfg.sat_ids.size()) {
        absl::StrAppend(&line, FixedWidth(cfg.sat_ids[idx], 3));
      } else {
        absl::StrAppend(&line, "  0");
      }
    }
    lines.push_back(std::move(line));
  }

  // Lines 8..12: '++' satellite accuracy exponents (5 lines x 17 satellites).
  for (int line_idx = 0; line_idx < 5; ++line_idx) {
    std::string line = "++       ";
    for (int col = 0; col < 17; ++col) {
      const size_t idx = static_cast<size_t>(line_idx * 17 + col);
      const int acc =
          (idx < cfg.sat_accuracies.size()) ? cfg.sat_accuracies[idx] : 0;
      absl::StrAppend(&line, absl::StrFormat("%3d", acc));
    }
    lines.push_back(std::move(line));
  }

  // Lines 13..14: '%c' file type and time system.
  lines.push_back(
      absl::StrCat("%c ", FixedWidth(cfg.file_type, 2), " cc ",
                   FixedWidth(cfg.time_system, 3),
                   " ccc cccc cccc cccc cccc ccccc ccccc ccccc ccccc"));
  lines.push_back(
      "%c cc cc ccc ccc cccc cccc cccc cccc ccccc ccccc ccccc ccccc");

  // Lines 15..16: '%f' floating-point base numbers.
  lines.push_back(
      absl::StrFormat("%%f %10.7f %12.9f  0.00000000000  0.000000000000000",
                      cfg.base_pos_vel, cfg.base_clk_rate));
  lines.push_back(
      "%f  0.0000000  0.000000000  0.00000000000  0.000000000000000");

  // Lines 17..18: '%i' integer fields.
  lines.push_back(
      "%i    0    0    0    0      0      0      0      0         0");
  lines.push_back(
      "%i    0    0    0    0      0      0      0      0         0");

  // Lines 19..22: '/*' comment lines.
  lines.push_back(
      "/* comment line 1                                           ");
  lines.push_back(
      "/* comment line 2                                           ");
  lines.push_back(
      "/* comment line 3                                           ");
  lines.push_back(
      "/* comment line 4                                           ");

  return lines;
}

std::string JoinLines(const std::vector<std::string>& lines,
                      std::string_view eol = "\n", bool trailing_eol = true) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    absl::StrAppend(&out, lines[i]);
    if (trailing_eol || i + 1 < lines.size()) {
      absl::StrAppend(&out, eol);
    }
  }
  return out;
}

std::string MakeEpochLine(std::string_view epoch_str) {
  return absl::StrCat("*  ", FixedWidth(epoch_str, 28));
}

std::string MakePositionLine(std::string_view vehicle_id, double x, double y,
                             double z, double clock,
                             std::optional<int> x_sdev = std::nullopt,
                             std::optional<int> y_sdev = std::nullopt,
                             std::optional<int> z_sdev = std::nullopt,
                             std::optional<int> clk_sdev = std::nullopt,
                             bool include_tail = true, char clock_event = ' ',
                             char clock_pred = ' ', char maneuver = ' ',
                             char orbit_pred = ' ') {
  std::string line = absl::StrFormat("P%s%14.6f%14.6f%14.6f%14.6f",
                                     FixedWidth(vehicle_id, 3), x, y, z, clock);
  if (!include_tail) {
    return line;
  }
  absl::StrAppend(
      &line, " ", x_sdev ? absl::StrFormat("%2d", *x_sdev) : "  ", " ",
      y_sdev ? absl::StrFormat("%2d", *y_sdev) : "  ", " ",
      z_sdev ? absl::StrFormat("%2d", *z_sdev) : "  ", " ",
      clk_sdev ? absl::StrFormat("%3d", *clk_sdev) : "   ", " ",
      std::string_view(&clock_event, 1), std::string_view(&clock_pred, 1), "  ",
      std::string_view(&maneuver, 1), std::string_view(&orbit_pred, 1));
  return line;
}

std::string MakeVelocityLine(std::string_view vehicle_id, double vx, double vy,
                             double vz, double clk_rate,
                             std::optional<int> vx_sdev = std::nullopt,
                             std::optional<int> vy_sdev = std::nullopt,
                             std::optional<int> vz_sdev = std::nullopt,
                             std::optional<int> clk_sdev = std::nullopt,
                             bool include_tail = true) {
  std::string line =
      absl::StrFormat("V%s%14.6f%14.6f%14.6f%14.6f", FixedWidth(vehicle_id, 3),
                      vx, vy, vz, clk_rate);
  if (!include_tail) {
    return line;
  }
  absl::StrAppend(&line, " ", vx_sdev ? absl::StrFormat("%2d", *vx_sdev) : "  ",
                  " ", vy_sdev ? absl::StrFormat("%2d", *vy_sdev) : "  ", " ",
                  vz_sdev ? absl::StrFormat("%2d", *vz_sdev) : "  ", " ",
                  clk_sdev ? absl::StrFormat("%3d", *clk_sdev) : "   ");
  return line;
}

std::string MakeCorrelationLine(std::string_view tag, int sdev_x, int sdev_y,
                                int sdev_z, int sdev_c, int xy, int xz, int xc,
                                int yz, int yc, int zc) {
  return absl::StrFormat("%s  %4d %4d %4d %7d %8d %8d %8d %8d %8d %8d",
                         FixedWidth(tag, 2), sdev_x, sdev_y, sdev_z, sdev_c, xy,
                         xz, xc, yz, yc, zc);
}

std::string ReadStringField(const coda_cursor& base, const char* path) {
  coda_cursor cur = base;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto(&cur, path));
  long len = -1;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cur, &len));
  if (len < 0) {
    return {};
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, '\0');
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_string(&cur, buf.data(), len + 1));
  return std::string(buf.data(), static_cast<size_t>(len));
}

int8_t ReadInt8Field(const coda_cursor& base, const char* path) {
  coda_cursor cur = base;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto(&cur, path));
  int8_t v = -1;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&cur, &v));
  return v;
}

uint8_t ReadUint8Field(const coda_cursor& base, const char* path) {
  coda_cursor cur = base;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto(&cur, path));
  uint8_t v = 255;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&cur, &v));
  return v;
}

int16_t ReadInt16Field(const coda_cursor& base, const char* path) {
  coda_cursor cur = base;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto(&cur, path));
  int16_t v = -1;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cur, &v));
  return v;
}

int32_t ReadInt32Field(const coda_cursor& base, const char* path) {
  coda_cursor cur = base;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto(&cur, path));
  int32_t v = -1;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cur, &v));
  return v;
}

double ReadDoubleField(const coda_cursor& base, const char* path) {
  coda_cursor cur = base;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto(&cur, path));
  double v = -1.0;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cur, &v));
  return v;
}

int GetFieldAvailable(const coda_cursor& base, const char* field_name) {
  long index = -1;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_index_from_name(
                              &base, field_name, &index));
  int available = -1;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                              &base, index, &available));
  return available;
}

std::string MakeMinimalValidSp3(char version, char pos_vel) {
  Sp3HeaderConfig cfg;
  cfg.version = version;
  cfg.pos_vel = pos_vel;
  cfg.num_epochs = 1;
  cfg.num_satellites = 1;
  cfg.sat_ids = {"G01"};
  cfg.sat_accuracies = {5};
  std::vector<std::string> lines = BuildSp3HeaderLines(cfg);
  lines.push_back(MakeEpochLine("2024 01 15 00 00  0.00000000"));
  lines.push_back(MakePositionLine("G01", 12345.678901, -23456.789012,
                                   15000.123456, 12.5, 5, 6, 7, 12));
  if (pos_vel == 'V') {
    lines.push_back(
        MakeVelocityLine("G01", 100.5, -200.25, 300.75, -0.5, 8, 9, 10, 15));
  }
  lines.push_back("EOF");
  return JoinLines(lines);
}

TEST(CodaSp3Test, RecognizeFileVersionsModesAndRejection) {
  const ScopedSp3TestEnv test_env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  EXPECT_STREQ("sp3", coda_type_get_format_name(coda_format_sp3));

  // 1. SP3a, SP3b, and SP3c in both 'P' and 'V' modes are recognized as SP3.
  struct VersionCase {
    char version;
    char pos_vel;
    coda_format expected_format;
  };
  constexpr VersionCase kVersionCases[] = {
      {.version = 'a', .pos_vel = 'P', .expected_format = coda_format_sp3},
      {.version = 'a', .pos_vel = 'V', .expected_format = coda_format_sp3},
      {.version = 'b', .pos_vel = 'P', .expected_format = coda_format_sp3},
      {.version = 'b', .pos_vel = 'V', .expected_format = coda_format_sp3},
      {.version = 'c', .pos_vel = 'P', .expected_format = coda_format_sp3},
      {.version = 'c', .pos_vel = 'V', .expected_format = coda_format_sp3},
      // coda-product.c:299 only matches versions 'a', 'b', 'c'; '#d' falls
      // back to coda_format_binary.
      {.version = 'd', .pos_vel = 'P', .expected_format = coda_format_binary},
  };

  for (const VersionCase& tc : kVersionCases) {
    const std::string content = MakeMinimalValidSp3(tc.version, tc.pos_vel);
    const TempFile file(
        absl::StrFormat("sp3_recognize_%c_%c.sp3", tc.version, tc.pos_vel));
    ASSERT_TRUE(file.WriteText(content));

    int64_t file_size = -1;
    coda_format fmt = coda_format_ascii;
    const char* pclass = "sentinel";
    const char* ptype = "sentinel";
    int version = 99;
    ASSERT_EQ(CODA_SUCCESS, coda_recognize_file(file.c_str(), &file_size, &fmt,
                                                &pclass, &ptype, &version));
    EXPECT_EQ(static_cast<int64_t>(content.size()), file_size);
    EXPECT_EQ(tc.expected_format, fmt);
    EXPECT_EQ(nullptr, pclass);
    EXPECT_EQ(nullptr, ptype);
    EXPECT_EQ(-1, version);
  }

  // 2. SP3d cannot be auto-opened via coda_open() without a definition.
  {
    const TempFile sp3d_file("sp3_d_unsupported.sp3");
    ASSERT_TRUE(sp3d_file.WriteText(MakeMinimalValidSp3('d', 'P')));
    ScopedProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open(sp3d_file.c_str(), product.OutParam()));
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
    EXPECT_EQ(nullptr, product.get());
  }

  // 3. Files failing SP3 line-1 detection shapes (<60 bytes, non-digit year,
  //    missing delimiter spaces, invalid mode) fall back to binary.
  constexpr std::string_view kNonSp3Headers[] = {
      "#cP2024 01 15 00 00  0.00000000       1 ORBIT IGS20 FIT",  // 55 bytes
      "#cX2024 01 15 00 00  0.00000000       1 ORBIT IGS20 FIT IGS ",
      "#cP202X 01 15 00 00  0.00000000       1 ORBIT IGS20 FIT IGS ",
      "#cP2024 01 15 00 00  0.00000000X      1 ORBIT IGS20 FIT IGS ",
      "#cP2024 01 15 00 00  0.00000000       1XORBIT IGS20 FIT IGS ",
  };
  for (size_t i = 0; i < std::size(kNonSp3Headers); ++i) {
    const TempFile file(absl::StrFormat("sp3_non_sp3_%zu.dat", i));
    ASSERT_TRUE(file.WriteText(kNonSp3Headers[i]));
    coda_format fmt = coda_format_sp3;
    ASSERT_EQ(CODA_SUCCESS, coda_recognize_file(file.c_str(), nullptr, &fmt,
                                                nullptr, nullptr, nullptr));
    EXPECT_EQ(coda_format_binary, fmt) << "index=" << i;
  }

  // 4. Non-existent path fails with CODA_ERROR_FILE_NOT_FOUND.
  const std::filesystem::path missing_path =
      std::filesystem::path(::testing::TempDir()) / "sp3_missing_file.sp3";
  std::error_code ec;
  std::filesystem::remove(missing_path, ec);
  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_recognize_file(missing_path.c_str(), nullptr, nullptr,
                                    nullptr, nullptr, nullptr));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(missing_path.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
  EXPECT_EQ(nullptr, product.get());
}

TEST(CodaSp3Test, OpenAndReadSp3cVelocityAndCorrelationsMmapAndNoMmap) {
  const ScopedSp3TestEnv test_env;

  // Build an SP3c 'V' file with 20 satellites (spanning 2 '+' and 2 '++'
  // lines), 2 epochs, populated/blank/short P, EP, V, EV lines.
  Sp3HeaderConfig cfg;
  cfg.version = 'c';
  cfg.pos_vel = 'V';
  cfg.num_epochs = 2;
  cfg.num_satellites = 20;
  cfg.sat_ids.clear();
  cfg.sat_accuracies.clear();
  for (int i = 1; i <= 20; ++i) {
    cfg.sat_ids.push_back(absl::StrFormat("G%02d", i));
    cfg.sat_accuracies.push_back(i + 2);
  }

  std::vector<std::string> lines = BuildSp3HeaderLines(cfg);
  // Epoch 0: full 80-char P + EP, full 73-char V + EV, plus blank-sdev P/V.
  lines.push_back(MakeEpochLine("2024 01 15 00 00  0.00000000"));
  lines.push_back(MakePositionLine("G01", 12345.678901, -23456.789012,
                                   15000.123456, 123.456789, 5, 6, 7, 12,
                                   /*include_tail=*/true, 'E', 'P', 'M', 'P'));
  lines.push_back(MakeCorrelationLine("EP", 101, 102, 103, 1004, -1234567,
                                      2345678, -3456789, 4567890, -5678901,
                                      6789012));
  lines.push_back(MakeVelocityLine("G01", 1000.123456, -2000.234567,
                                   3000.345678, -0.123456, 8, 9, 10, 15));
  lines.push_back(MakeCorrelationLine("EV", 201, 202, 203, 2004, 1111111,
                                      -2222222, 3333333, -4444444, 5555555,
                                      -6666666));
  // Blank-sdev 80-char P + blank 80-char EP, blank-sdev 73-char V + blank EV.
  lines.push_back(MakePositionLine("G02", 100.0, 200.0, 300.0, 4.0,
                                   std::nullopt, std::nullopt, std::nullopt,
                                   std::nullopt, /*include_tail=*/true));
  lines.push_back(std::string("EP") + std::string(78, ' '));
  lines.push_back(MakeVelocityLine("G02", 1.0, 2.0, 3.0, 0.25, std::nullopt,
                                   std::nullopt, std::nullopt, std::nullopt,
                                   /*include_tail=*/true));
  lines.push_back(std::string("EV") + std::string(78, ' '));

  // Epoch 1: short 60-char P/V (no trailing sdev/flags) + truncated EP/EV,
  // plus a second P/V pair without any EP/EV correlation lines.
  lines.push_back(MakeEpochLine("2024 01 15 00 15  0.00000000"));
  lines.push_back(MakePositionLine("G01", 111.0, 222.0, 333.0, 444.0,
                                   std::nullopt, std::nullopt, std::nullopt,
                                   std::nullopt, /*include_tail=*/false));
  lines.push_back("EP  ");
  lines.push_back(MakeVelocityLine("G01", 11.0, 22.0, 33.0, 44.0, std::nullopt,
                                   std::nullopt, std::nullopt, std::nullopt,
                                   /*include_tail=*/false));
  lines.push_back("EV  ");
  lines.push_back(MakePositionLine("G20", 555.0, 666.0, 777.0, 999999.999999,
                                   std::nullopt, std::nullopt, std::nullopt,
                                   std::nullopt, /*include_tail=*/false));
  lines.push_back(MakeVelocityLine("G20", 55.0, 66.0, 77.0, 0.0, std::nullopt,
                                   std::nullopt, std::nullopt, std::nullopt,
                                   /*include_tail=*/false));
  lines.push_back("EOF");

  double expected_start_time = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_time_parts_to_double(2024, 1, 15, 0, 0, 0, 0,
                                                    &expected_start_time));
  double expected_epoch1_time = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_time_parts_to_double(2024, 1, 15, 0, 15, 0, 0,
                                                    &expected_epoch1_time));

  struct ModeCase {
    int use_mmap;
    std::string_view eol;
    bool trailing_eol;
  };
  constexpr ModeCase kModeCases[] = {
      {.use_mmap = 1, .eol = "\r\n", .trailing_eol = true},
      {.use_mmap = 0, .eol = "\n", .trailing_eol = false},
  };

  for (const ModeCase& mode : kModeCases) {
    coda_set_option_use_mmap(mode.use_mmap);
    const TempFile file(
        absl::StrFormat("sp3_c_vel_mmap_%d.sp3", mode.use_mmap));
    ASSERT_TRUE(file.WriteText(JoinLines(lines, mode.eol, mode.trailing_eol)));

    ScopedCodaSession session;
    ASSERT_EQ(CODA_SUCCESS, session.Init());
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

    coda_format fmt = coda_format_ascii;
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(product.get(), &fmt));
    EXPECT_EQ(coda_format_sp3, fmt);

    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

    // 1. Verify header fields.
    coda_cursor hdr = root;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&hdr, "header"));
    EXPECT_EQ("V", ReadStringField(hdr, "pos_vel"));
    EXPECT_DOUBLE_EQ(expected_start_time,
                     ReadDoubleField(hdr, "datetime_start"));
    {
      coda_cursor dt = hdr;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&dt, "datetime_start"));
      coda_special_type stype = coda_special_no_data;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&dt, &stype));
      EXPECT_EQ(coda_special_time, stype);
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&dt));
      EXPECT_EQ("2024 01 15 00 00  0.00000000", ReadStringField(dt, "."));
    }
    EXPECT_EQ(2, ReadInt32Field(hdr, "num_epochs"));
    EXPECT_EQ("ORBIT", ReadStringField(hdr, "data_used"));
    EXPECT_EQ("IGS20", ReadStringField(hdr, "coordinate_sys"));
    EXPECT_EQ("FIT", ReadStringField(hdr, "orbit_type"));
    EXPECT_EQ("IGS ", ReadStringField(hdr, "agency"));
    EXPECT_EQ(2297, ReadInt16Field(hdr, "gps_week"));
    EXPECT_DOUBLE_EQ(86400.0, ReadDoubleField(hdr, "sec_of_week"));
    EXPECT_DOUBLE_EQ(900.0, ReadDoubleField(hdr, "epoch_interval"));
    EXPECT_EQ(60324, ReadInt32Field(hdr, "mjd_start"));
    EXPECT_DOUBLE_EQ(0.0, ReadDoubleField(hdr, "frac_day"));
    EXPECT_EQ(20, ReadUint8Field(hdr, "num_satellites"));
    EXPECT_EQ("M ", ReadStringField(hdr, "file_type"));
    EXPECT_EQ("GPS", ReadStringField(hdr, "time_system"));
    EXPECT_DOUBLE_EQ(1.25, ReadDoubleField(hdr, "base_pos_vel"));
    EXPECT_DOUBLE_EQ(1.025, ReadDoubleField(hdr, "base_clk_rate"));

    // Verify satellite IDs and accuracies across both '+' and '++' lines.
    coda_cursor sat_id_arr = hdr;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&sat_id_arr, "sat_id"));
    long num_sats = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_num_elements(&sat_id_arr, &num_sats));
    EXPECT_EQ(20, num_sats);
    EXPECT_EQ("G01", ReadStringField(hdr, "sat_id[0]"));
    EXPECT_EQ("G17", ReadStringField(hdr, "sat_id[16]"));
    EXPECT_EQ("G18", ReadStringField(hdr, "sat_id[17]"));
    EXPECT_EQ("G20", ReadStringField(hdr, "sat_id[19]"));

    std::array<int16_t, 20> accuracies{};
    accuracies.fill(-1);
    coda_cursor acc_arr = hdr;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&acc_arr, "sat_accuracy"));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int16_array(&acc_arr, accuracies.data(),
                                           coda_array_ordering_c));
    EXPECT_EQ(3, accuracies[0]);
    EXPECT_EQ(19, accuracies[16]);
    EXPECT_EQ(20, accuracies[17]);
    EXPECT_EQ(22, accuracies[19]);

    // 2. Verify Epoch 0 record[0] P/EP/V/EV fields and units.
    coda_cursor rec0 = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&rec0, "record[0]"));
    EXPECT_DOUBLE_EQ(expected_start_time, ReadDoubleField(rec0, "epoch"));
    EXPECT_EQ(1, GetFieldAvailable(rec0, "vel_rate"));

    coda_cursor p0 = rec0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&p0, "pos_clk[0]"));
    EXPECT_EQ("G01", ReadStringField(p0, "vehicle_id"));
    EXPECT_DOUBLE_EQ(12345.678901, ReadDoubleField(p0, "x_coordinate"));
    EXPECT_DOUBLE_EQ(-23456.789012, ReadDoubleField(p0, "y_coordinate"));
    EXPECT_DOUBLE_EQ(15000.123456, ReadDoubleField(p0, "z_coordinate"));
    EXPECT_DOUBLE_EQ(123.456789, ReadDoubleField(p0, "clock"));
    EXPECT_EQ(5, ReadInt8Field(p0, "x_sdev"));
    EXPECT_EQ(6, ReadInt8Field(p0, "y_sdev"));
    EXPECT_EQ(7, ReadInt8Field(p0, "z_sdev"));
    EXPECT_EQ(12, ReadInt16Field(p0, "clock_sdev"));
    EXPECT_EQ("E", ReadStringField(p0, "clock_event_flag"));
    EXPECT_EQ("P", ReadStringField(p0, "clock_pred_flag"));
    EXPECT_EQ("M", ReadStringField(p0, "maneuver_flag"));
    EXPECT_EQ("P", ReadStringField(p0, "orbit_pred_flag"));
    EXPECT_EQ(1, GetFieldAvailable(p0, "corr"));

    coda_cursor p0_corr = p0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&p0_corr, "corr"));
    EXPECT_EQ(101, ReadInt16Field(p0_corr, "x_sdev"));
    EXPECT_EQ(102, ReadInt16Field(p0_corr, "y_sdev"));
    EXPECT_EQ(103, ReadInt16Field(p0_corr, "z_sdev"));
    EXPECT_EQ(1004, ReadInt32Field(p0_corr, "clock_sdev"));
    EXPECT_EQ(-1234567, ReadInt32Field(p0_corr, "xy_corr"));
    EXPECT_EQ(2345678, ReadInt32Field(p0_corr, "xz_corr"));
    EXPECT_EQ(-3456789, ReadInt32Field(p0_corr, "xc_corr"));
    EXPECT_EQ(4567890, ReadInt32Field(p0_corr, "yz_corr"));
    EXPECT_EQ(-5678901, ReadInt32Field(p0_corr, "yc_corr"));
    EXPECT_EQ(6789012, ReadInt32Field(p0_corr, "zc_corr"));

    // Verify units on P and EP fields.
    {
      coda_cursor x_cur = p0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&x_cur, "x_coordinate"));
      coda_type* x_type = nullptr;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(&x_cur, &x_type));
      const char* unit = nullptr;
      ASSERT_EQ(CODA_SUCCESS, coda_type_get_unit(x_type, &unit));
      EXPECT_STREQ("km", unit);

      coda_cursor ep_clk = p0_corr;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&ep_clk, "clock_sdev"));
      coda_type* ep_clk_type = nullptr;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(&ep_clk, &ep_clk_type));
      ASSERT_EQ(CODA_SUCCESS, coda_type_get_unit(ep_clk_type, &unit));
      EXPECT_STREQ("ps", unit);
    }

    coda_cursor v0 = rec0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&v0, "vel_rate[0]"));
    EXPECT_EQ("G01", ReadStringField(v0, "vehicle_id"));
    EXPECT_DOUBLE_EQ(1000.123456, ReadDoubleField(v0, "x_velocity"));
    EXPECT_DOUBLE_EQ(-2000.234567, ReadDoubleField(v0, "y_velocity"));
    EXPECT_DOUBLE_EQ(3000.345678, ReadDoubleField(v0, "z_velocity"));
    EXPECT_DOUBLE_EQ(-0.123456, ReadDoubleField(v0, "clock_rate"));
    EXPECT_EQ(8, ReadInt8Field(v0, "xvel_sdev"));
    EXPECT_EQ(9, ReadInt8Field(v0, "yvel_sdev"));
    EXPECT_EQ(10, ReadInt8Field(v0, "zvel_sdev"));
    EXPECT_EQ(15, ReadInt16Field(v0, "clkrate_sdev"));
    EXPECT_EQ(1, GetFieldAvailable(v0, "corr"));

    coda_cursor v0_corr = v0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&v0_corr, "corr"));
    EXPECT_EQ(201, ReadInt16Field(v0_corr, "xvel_sdev"));
    EXPECT_EQ(202, ReadInt16Field(v0_corr, "yvel_sdev"));
    EXPECT_EQ(203, ReadInt16Field(v0_corr, "zvel_sdev"));
    EXPECT_EQ(2004, ReadInt16Field(v0_corr, "clkrate_sdev"));
    EXPECT_EQ(1111111, ReadInt32Field(v0_corr, "xy_corr"));
    EXPECT_EQ(-2222222, ReadInt32Field(v0_corr, "xz_corr"));
    EXPECT_EQ(3333333, ReadInt32Field(v0_corr, "xc_corr"));
    EXPECT_EQ(-4444444, ReadInt32Field(v0_corr, "yz_corr"));
    EXPECT_EQ(5555555, ReadInt32Field(v0_corr, "yc_corr"));
    EXPECT_EQ(-6666666, ReadInt32Field(v0_corr, "zc_corr"));

    // 3. Verify blank-sdev P/EP and V/EV default to 0 / ' '.
    coda_cursor p1 = rec0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&p1, "pos_clk[1]"));
    EXPECT_EQ("G02", ReadStringField(p1, "vehicle_id"));
    EXPECT_EQ(0, ReadInt8Field(p1, "x_sdev"));
    EXPECT_EQ(0, ReadInt8Field(p1, "y_sdev"));
    EXPECT_EQ(0, ReadInt8Field(p1, "z_sdev"));
    EXPECT_EQ(0, ReadInt16Field(p1, "clock_sdev"));
    EXPECT_EQ(" ", ReadStringField(p1, "clock_event_flag"));
    EXPECT_EQ(0, ReadInt16Field(p1, "corr/x_sdev"));
    EXPECT_EQ(0, ReadInt32Field(p1, "corr/zc_corr"));

    coda_cursor v1 = rec0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&v1, "vel_rate[1]"));
    EXPECT_EQ(0, ReadInt8Field(v1, "xvel_sdev"));
    EXPECT_EQ(0, ReadInt16Field(v1, "clkrate_sdev"));
    EXPECT_EQ(0, ReadInt16Field(v1, "corr/xvel_sdev"));
    EXPECT_EQ(0, ReadInt32Field(v1, "corr/zc_corr"));

    // 4. Verify Epoch 1 short 60-char P/V + truncated EP/EV + absent corr.
    coda_cursor rec1 = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&rec1, "record[1]"));
    EXPECT_DOUBLE_EQ(expected_epoch1_time, ReadDoubleField(rec1, "epoch"));

    coda_cursor r1p0 = rec1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&r1p0, "pos_clk[0]"));
    EXPECT_EQ(0, ReadInt8Field(r1p0, "x_sdev"));
    EXPECT_EQ(0, ReadInt16Field(r1p0, "clock_sdev"));
    EXPECT_EQ(" ", ReadStringField(r1p0, "orbit_pred_flag"));
    EXPECT_EQ(0, ReadInt16Field(r1p0, "corr/x_sdev"));
    EXPECT_EQ(0, ReadInt32Field(r1p0, "corr/zc_corr"));

    coda_cursor r1v0 = rec1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&r1v0, "vel_rate[0]"));
    EXPECT_EQ(0, ReadInt8Field(r1v0, "xvel_sdev"));
    EXPECT_EQ(0, ReadInt16Field(r1v0, "clkrate_sdev"));
    EXPECT_EQ(0, ReadInt16Field(r1v0, "corr/clkrate_sdev"));
    EXPECT_EQ(0, ReadInt32Field(r1v0, "corr/zc_corr"));

    coda_cursor r1p1 = rec1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&r1p1, "pos_clk[1]"));
    EXPECT_EQ("G20", ReadStringField(r1p1, "vehicle_id"));
    EXPECT_EQ(0, GetFieldAvailable(r1p1, "corr"));

    coda_cursor r1v1 = rec1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&r1v1, "vel_rate[1]"));
    EXPECT_EQ("G20", ReadStringField(r1v1, "vehicle_id"));
    EXPECT_EQ(0, GetFieldAvailable(r1v1, "corr"));
  }
}

TEST(CodaSp3Test, OpenAndReadPositionOnlyAndZeroEpochFiles) {
  const ScopedSp3TestEnv test_env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. SP3a position-only file with 2 epochs leaves vel_rate unavailable.
  {
    Sp3HeaderConfig cfg;
    cfg.version = 'a';
    cfg.pos_vel = 'P';
    cfg.num_epochs = 2;
    cfg.num_satellites = 1;
    cfg.sat_ids = {"G01"};
    cfg.sat_accuracies = {4};
    std::vector<std::string> lines = BuildSp3HeaderLines(cfg);
    lines.push_back(MakeEpochLine("2024 01 15 00 00  0.00000000"));
    lines.push_back(MakePositionLine("G01", 10.0, 20.0, 30.0, 1.0, std::nullopt,
                                     std::nullopt, std::nullopt, std::nullopt,
                                     false));
    lines.push_back(MakeEpochLine("2024 01 15 00 15  0.00000000"));
    lines.push_back(MakePositionLine("G01", 40.0, 50.0, 60.0, 2.0, std::nullopt,
                                     std::nullopt, std::nullopt, std::nullopt,
                                     false));
    lines.push_back("EOF");

    const TempFile file("sp3_a_pos_only.sp3");
    ASSERT_TRUE(file.WriteText(JoinLines(lines)));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
    EXPECT_EQ("P", ReadStringField(root, "header/pos_vel"));

    coda_cursor rec0 = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&rec0, "record[0]"));
    EXPECT_EQ(0, GetFieldAvailable(rec0, "vel_rate"));
    EXPECT_DOUBLE_EQ(10.0, ReadDoubleField(rec0, "pos_clk[0]/x_coordinate"));

    coda_cursor rec1 = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&rec1, "record[1]"));
    EXPECT_EQ(0, GetFieldAvailable(rec1, "vel_rate"));
    EXPECT_DOUBLE_EQ(40.0, ReadDoubleField(rec1, "pos_clk[0]/x_coordinate"));
  }

  // 2. SP3b with 0 satellites and 0 epochs (immediate EOF trailer).
  {
    Sp3HeaderConfig cfg;
    cfg.version = 'b';
    cfg.pos_vel = 'P';
    cfg.num_epochs = 0;
    cfg.num_satellites = 0;
    cfg.sat_ids.clear();
    cfg.sat_accuracies.clear();
    std::vector<std::string> lines = BuildSp3HeaderLines(cfg);
    lines.push_back("EOF");

    const TempFile file("sp3_b_zero_epochs.sp3");
    ASSERT_TRUE(file.WriteText(JoinLines(lines)));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

    coda_cursor cur;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cur, product.get()));
    EXPECT_EQ(0, ReadUint8Field(cur, "header/num_satellites"));

    long count = -1;
    coda_cursor sat_cur = cur;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&sat_cur, "header/sat_id"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&sat_cur, &count));
    EXPECT_EQ(0, count);

    coda_cursor rec_cur = cur;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&rec_cur, "record"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&rec_cur, &count));
    EXPECT_EQ(0, count);
  }
}

TEST(CodaSp3Test, DirectSp3ReopenSp3dAndLifecycleEdgeCases) {
  const ScopedSp3TestEnv test_env;

  // 1. Calling coda_sp3_done() before any SP3 product is opened is a no-op.
  coda_sp3_done();

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 2. Direct coda_sp3_reopen() on an SP3d ('#d') file opened via
  // coda_bin_open.
  {
    const std::string sp3d_content = MakeMinimalValidSp3('d', 'P');
    const TempFile sp3d_file("sp3_direct_sp3d.sp3");
    ASSERT_TRUE(sp3d_file.WriteText(sp3d_content));

    coda_product* prod = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_bin_open(sp3d_file.c_str(),
                            static_cast<int64_t>(sp3d_content.size()), &prod));
    ASSERT_NE(nullptr, prod);
    ASSERT_EQ(CODA_SUCCESS, coda_sp3_reopen(&prod));
    ScopedProduct sp3_guard(prod);

    EXPECT_EQ(coda_format_sp3, prod->format);
    coda_cursor cur;
    ASSERT_EQ(CODA_SUCCESS, coda_sp3_cursor_set_product(&cur, prod));
    EXPECT_EQ("P", ReadStringField(cur, "header/pos_vel"));
    EXPECT_EQ("G01", ReadStringField(cur, "record[0]/pos_clk[0]/vehicle_id"));
    EXPECT_DOUBLE_EQ(12345.678901,
                     ReadDoubleField(cur, "record[0]/pos_clk[0]/x_coordinate"));
    EXPECT_EQ(CODA_SUCCESS, coda_sp3_close(sp3_guard.release()));
  }

  // 3. coda_sp3_reopen() when line 1 is shorter than 60 bytes fails cleanly.
  {
    constexpr std::string_view kShortLine1 = "#cP2024\n";
    const TempFile short_file("sp3_short_line1.sp3");
    ASSERT_TRUE(short_file.WriteText(kShortLine1));

    coda_product* prod = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_bin_open(short_file.c_str(),
                            static_cast<int64_t>(kShortLine1.size()), &prod));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_sp3_reopen(&prod));
    EXPECT_EQ(CODA_ERROR_FILE_READ, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("header line length"));
  }

  // 4. coda_sp3_reopen() when fopen() fails after file removal returns
  //    CODA_ERROR_FILE_OPEN.
  {
    const std::string sp3c_content = MakeMinimalValidSp3('c', 'P');
    const TempFile removed_file("sp3_removed_before_reopen.sp3");
    ASSERT_TRUE(removed_file.WriteText(sp3c_content));

    coda_product* prod = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_bin_open(removed_file.c_str(),
                            static_cast<int64_t>(sp3c_content.size()), &prod));
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::remove(removed_file.path(), ec));

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_sp3_reopen(&prod));
    EXPECT_EQ(CODA_ERROR_FILE_OPEN, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("could not open file"));
  }
}

TEST(CodaSp3Test, MalformedHeaderErrors) {
  const ScopedSp3TestEnv test_env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  struct HeaderErrorCase {
    std::string_view name;
    size_t line_index;
    std::string_view replacement_line;
    int expected_errno;
    std::string_view expected_substr;
    bool allow_upstream_leak = false;
  };

  const HeaderErrorCase kCases[] = {
      {.name = "line1_bad_num_epochs",
       .line_index = 0,
       .replacement_line =
           "#cP2024 01 15 00 00  0.00000000 BADEPOC ORBIT IGS20 FIT IGS ",
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "invalid format for ascii integer"},
      {.name = "line2_short",
       .line_index = 1,
       .replacement_line = "## 2297",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "header line length"},
      {.name = "line2_bad_lead",
       .line_index = 1,
       .replacement_line =
           "XX 2297  86400.00000000   900.00000000 60324 0.0000000000000",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "invalid lead characters"},
      {.name = "line2_bad_gps_week",
       .line_index = 1,
       .replacement_line =
           "## ABCD  86400.00000000   900.00000000 60324 0.0000000000000",
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "line2_bad_sec_of_week",
       .line_index = 1,
       .replacement_line =
           "## 2297 BAD_SEC_OF_WEEK   900.00000000 60324 0.0000000000000",
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "floating point"},
      {.name = "line2_bad_epoch_interval",
       .line_index = 1,
       .replacement_line =
           "## 2297  86400.00000000 BAD_INTERVAL!! 60324 0.0000000000000",
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "floating point"},
      {.name = "line2_bad_mjd_start",
       .line_index = 1,
       .replacement_line =
           "## 2297  86400.00000000   900.00000000 BADMJ 0.0000000000000",
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "line2_bad_frac_day",
       .line_index = 1,
       .replacement_line =
           "## 2297  86400.00000000   900.00000000 60324 BAD_FRAC_DAY!!!",
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "floating point"},
      {.name = "line3_short",
       .line_index = 2,
       .replacement_line = "+    1",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "header line length"},
      {.name = "line3_bad_lead",
       .line_index = 2,
       .replacement_line =
           "X    1   G01  0  0  0  0  0  0  0  0  0  0  0  0  0  0  0  0",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "invalid lead characters"},
      {.name = "line3_bad_num_satellites",
       .line_index = 2,
       .replacement_line =
           "+   XX   G01  0  0  0  0  0  0  0  0  0  0  0  0  0  0  0  0",
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "line4_short",
       .line_index = 3,
       .replacement_line = "+",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "header line length"},
      {.name = "line4_bad_lead",
       .line_index = 3,
       .replacement_line =
           "+ X        0  0  0  0  0  0  0  0  0  0  0  0  0  0  0  0  0",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "invalid lead characters"},
      {.name = "line8_short",
       .line_index = 7,
       .replacement_line = "++",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "header line length"},
      {.name = "line8_bad_lead",
       .line_index = 7,
       .replacement_line =
           "++ X       5  0  0  0  0  0  0  0  0  0  0  0  0  0  0  0  0",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "invalid lead characters"},
      {.name = "line8_bad_sat_accuracy",
       .line_index = 7,
       .replacement_line =
           "++        XX  0  0  0  0  0  0  0  0  0  0  0  0  0  0  0  0",
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer",
       .allow_upstream_leak = true},
      {.name = "line13_short",
       .line_index = 12,
       .replacement_line = "%c M ",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "header line length"},
      {.name = "line13_bad_lead",
       .line_index = 12,
       .replacement_line =
           "%x M  cc GPS ccc cccc cccc cccc cccc ccccc ccccc ccccc ccccc",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "invalid lead characters"},
      {.name = "line14_short",
       .line_index = 13,
       .replacement_line = "%c cc",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "header line length"},
      {.name = "line14_bad_lead",
       .line_index = 13,
       .replacement_line =
           "%x cc cc ccc ccc cccc cccc cccc cccc ccccc ccccc ccccc ccccc",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "invalid lead characters"},
      {.name = "line15_short",
       .line_index = 14,
       .replacement_line = "%f  1.25",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "header line length"},
      {.name = "line15_bad_lead",
       .line_index = 14,
       .replacement_line =
           "%x  1.2500000  1.025000000  0.00000000000  0.000000000000000",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "invalid lead characters"},
      {.name = "line15_bad_base_pos_vel",
       .line_index = 14,
       .replacement_line =
           "%f BAD_BASE1!  1.025000000  0.00000000000  0.000000000000000",
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "floating point"},
      {.name = "line15_bad_base_clk_rate",
       .line_index = 14,
       .replacement_line =
           "%f  1.2500000 BAD_BASE2!!!  0.00000000000  0.000000000000000",
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "floating point"},
  };

  for (const HeaderErrorCase& tc : kCases) {
    Sp3HeaderConfig cfg;
    cfg.num_satellites = 1;
    cfg.sat_ids = {"G01"};
    cfg.sat_accuracies = {5};
    std::vector<std::string> lines = BuildSp3HeaderLines(cfg);
    lines[tc.line_index] = std::string(tc.replacement_line);
    lines.push_back("EOF");

    const TempFile file(absl::StrCat("sp3_bad_hdr_", tc.name, ".sp3"));
    ASSERT_TRUE(file.WriteText(JoinLines(lines)));

    ScopedProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    int rc = 0;
    if (tc.allow_upstream_leak) {
      // Upstream coda-sp3.c:1129-1134 returns -1 without deleting `array`.
      const absl::LeakCheckDisabler leak_check_disabler;
      rc = coda_open(file.c_str(), product.OutParam());
    } else {
      rc = coda_open(file.c_str(), product.OutParam());
    }
    EXPECT_EQ(-1, rc) << tc.name;
    EXPECT_EQ(nullptr, product.get()) << tc.name;
    EXPECT_EQ(tc.expected_errno, coda_errno) << tc.name;
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr(tc.expected_substr))
        << tc.name;
  }
}

TEST(CodaSp3Test, MalformedRecordErrors) {
  const ScopedSp3TestEnv test_env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  struct RecordErrorCase {
    std::string_view name;
    char pos_vel;
    std::vector<std::string> body_lines;
    int expected_errno;
    std::string_view expected_substr;
  };

  const std::string valid_epoch = MakeEpochLine("2024 01 15 00 00  0.00000000");
  const std::string valid_p = MakePositionLine("G01", 1.0, 2.0, 3.0, 4.0, 5, 6,
                                               7, 12, /*include_tail=*/true);
  const std::string valid_v = MakeVelocityLine("G01", 1.0, 2.0, 3.0, 4.0, 5, 6,
                                               7, 12, /*include_tail=*/true);
  const std::string base_ep =
      MakeCorrelationLine("EP", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  const std::string base_ev =
      MakeCorrelationLine("EV", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);

  auto replace_slice = [](std::string s, size_t pos, std::string_view val) {
    s.replace(pos, val.size(), val);
    return s;
  };

  const std::vector<RecordErrorCase> kCases = {
      {.name = "missing_eof_after_header",
       .pos_vel = 'P',
       .body_lines = {"END"},
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "invalid line"},
      {.name = "missing_eof_after_p_record",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, valid_p},
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "invalid line"},
      {.name = "epoch_line_too_short",
       .pos_vel = 'P',
       .body_lines = {"*  2024 01 15", "EOF"},
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "record line length"},
      {.name = "p_without_epoch",
       .pos_vel = 'P',
       .body_lines = {valid_p, "EOF"},
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "without Epoch Header"},
      {.name = "p_line_too_short",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, "PG01   1.0", "EOF"},
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "record line length"},
      {.name = "p_bad_x",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, replace_slice(valid_p, 4, "BAD_X_VALUE!!!"),
                      "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "floating point"},
      {.name = "p_bad_y",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, replace_slice(valid_p, 18, "BAD_Y_VALUE!!!"),
                      "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "floating point"},
      {.name = "p_bad_z",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, replace_slice(valid_p, 32, "BAD_Z_VALUE!!!"),
                      "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "floating point"},
      {.name = "p_bad_clock",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, replace_slice(valid_p, 46, "BAD_CLK_VALUE!"),
                      "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "floating point"},
      {.name = "p_bad_x_sdev",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, replace_slice(valid_p, 61, "XX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "p_bad_y_sdev",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, replace_slice(valid_p, 64, "XX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "p_bad_z_sdev",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, replace_slice(valid_p, 67, "XX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "p_bad_clock_sdev",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, replace_slice(valid_p, 70, "XXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "v_disallowed_in_p_mode",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, valid_v, "EOF"},
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "not allowed due to header"},
      {.name = "v_without_epoch",
       .pos_vel = 'V',
       .body_lines = {valid_v, "EOF"},
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "without Epoch Header"},
      {.name = "v_line_too_short",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, "VG01   1.0", "EOF"},
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "record line length"},
      {.name = "v_bad_vx",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, replace_slice(valid_v, 4, "BAD_VX_VALUE!!"),
                      "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "floating point"},
      {.name = "v_bad_vy",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, replace_slice(valid_v, 18, "BAD_VY_VALUE!!"),
                      "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "floating point"},
      {.name = "v_bad_vz",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, replace_slice(valid_v, 32, "BAD_VZ_VALUE!!"),
                      "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "floating point"},
      {.name = "v_bad_clock_rate",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, replace_slice(valid_v, 46, "BAD_CLKRATE!!!"),
                      "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "floating point"},
      {.name = "v_bad_xvel_sdev",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, replace_slice(valid_v, 61, "XX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "v_bad_yvel_sdev",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, replace_slice(valid_v, 64, "XX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "v_bad_zvel_sdev",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, replace_slice(valid_v, 67, "XX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "v_bad_clkrate_sdev",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, replace_slice(valid_v, 70, "XXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ep_without_p",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, base_ep, "EOF"},
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "without Position and Clock Record"},
      {.name = "ep_bad_x_sdev",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, valid_p, replace_slice(base_ep, 4, "XXXX"),
                      "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ep_bad_y_sdev",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, valid_p, replace_slice(base_ep, 9, "XXXX"),
                      "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ep_bad_z_sdev",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, valid_p, replace_slice(base_ep, 14, "XXXX"),
                      "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ep_bad_clock_sdev",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, valid_p,
                      replace_slice(base_ep, 19, "XXXXXXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ep_bad_xy_corr",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, valid_p,
                      replace_slice(base_ep, 27, "XXXXXXXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ep_bad_xz_corr",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, valid_p,
                      replace_slice(base_ep, 36, "XXXXXXXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ep_bad_xc_corr",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, valid_p,
                      replace_slice(base_ep, 45, "XXXXXXXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ep_bad_yz_corr",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, valid_p,
                      replace_slice(base_ep, 54, "XXXXXXXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ep_bad_yc_corr",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, valid_p,
                      replace_slice(base_ep, 63, "XXXXXXXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ep_bad_zc_corr",
       .pos_vel = 'P',
       .body_lines = {valid_epoch, valid_p,
                      replace_slice(base_ep, 72, "XXXXXXXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ev_without_v",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, valid_p, base_ev, "EOF"},
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_substr = "without Velocity and Rate Record"},
      {.name = "ev_bad_xvel_sdev",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, valid_v, replace_slice(base_ev, 4, "XXXX"),
                      "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ev_bad_yvel_sdev",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, valid_v, replace_slice(base_ev, 9, "XXXX"),
                      "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ev_bad_zvel_sdev",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, valid_v, replace_slice(base_ev, 14, "XXXX"),
                      "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ev_bad_clkrate_sdev",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, valid_v,
                      replace_slice(base_ev, 19, "XXXXXXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ev_bad_xy_corr",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, valid_v,
                      replace_slice(base_ev, 27, "XXXXXXXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ev_bad_xz_corr",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, valid_v,
                      replace_slice(base_ev, 36, "XXXXXXXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ev_bad_xc_corr",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, valid_v,
                      replace_slice(base_ev, 45, "XXXXXXXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ev_bad_yz_corr",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, valid_v,
                      replace_slice(base_ev, 54, "XXXXXXXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ev_bad_yc_corr",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, valid_v,
                      replace_slice(base_ev, 63, "XXXXXXXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
      {.name = "ev_bad_zc_corr",
       .pos_vel = 'V',
       .body_lines = {valid_epoch, valid_v,
                      replace_slice(base_ev, 72, "XXXXXXXX"), "EOF"},
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_substr = "ascii integer"},
  };

  for (const RecordErrorCase& tc : kCases) {
    Sp3HeaderConfig cfg;
    cfg.pos_vel = tc.pos_vel;
    cfg.num_satellites = 1;
    cfg.sat_ids = {"G01"};
    cfg.sat_accuracies = {5};
    std::vector<std::string> lines = BuildSp3HeaderLines(cfg);
    lines.insert(lines.end(), tc.body_lines.begin(), tc.body_lines.end());

    const TempFile file(absl::StrCat("sp3_bad_rec_", tc.name, ".sp3"));
    ASSERT_TRUE(file.WriteText(JoinLines(lines)));

    ScopedProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open(file.c_str(), product.OutParam())) << tc.name;
    EXPECT_EQ(nullptr, product.get()) << tc.name;
    EXPECT_EQ(tc.expected_errno, coda_errno) << tc.name;
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr(tc.expected_substr))
        << tc.name;
  }
}

// TODO(schwehr): 98.0% line coverage in libcoda/coda-sp3.c (1347/1374);
// remaining 27 missed lines are malloc/strdup OOM checks in coda_sp3_reopen()
// and epoch record allocation.

}  // namespace
}  // namespace stcorp_coda
