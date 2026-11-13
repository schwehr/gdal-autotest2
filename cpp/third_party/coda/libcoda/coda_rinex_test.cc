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
// Tests CODA RINEX file recognition, header/record ingestion, cursor reading,
// and error handling (libcoda/coda-rinex.c).
//
// google3 build caveats:
//   - Built on little-endian Linux (WORDS_BIGENDIAN branches compiled out).
//   - CODA's RINEX backend only accepts version 3.00 for Observation ('O') and
//     Navigation ('N') files, versions 2.00/3.00 for Clock ('C') files, and
//     rejects RINEX v2.x Obs/Nav and Meteorological ('M') files with
//     CODA_ERROR_UNSUPPORTED_PRODUCT.
//   - Upstream coda_rinex_close() never frees product->mem_ptr, and
//     coda_open() / coda_recognize_file() / handle_observation_definition()
//     leak internal allocations on RINEX files. Opened products free
//     product->mem_ptr via ScopedRinexProduct before coda_close(), and all
//     coda_open(), coda_rinex_reopen(), and coda_recognize_file() calls are
//     wrapped in CallAllowingUpstreamRinexLeak() using an unconditional
//     absl::LeakCheckDisabler (covering both tcmalloc HeapLeakChecker and
//     LSan).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-rinex.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-rinex.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-bin.h"
#include "third_party/stcorp_coda/libcoda/coda-rinex.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

// Isolates CODA environment variables, definition path, coda_option_use_mmap,
// and thread-local coda_errno across every test.
class ScopedRinexTestEnv {
 public:
  ScopedRinexTestEnv()
      : env_def_("CODA_DEFINITION"),
        env_leap_("CODA_LEAP_SECOND_TABLE"),
        saved_use_mmap_(coda_get_option_use_mmap()) {
    env_def_.Unset();
    env_leap_.Unset();
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  ~ScopedRinexTestEnv() {
    coda_set_option_use_mmap(saved_use_mmap_);
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  ScopedRinexTestEnv(const ScopedRinexTestEnv&) = delete;
  ScopedRinexTestEnv& operator=(const ScopedRinexTestEnv&) = delete;

 private:
  ScopedEnvVar env_def_;
  ScopedEnvVar env_leap_;
  int saved_use_mmap_;
};

// RAII wrapper for a RINEX coda_product* that frees product->mem_ptr before
// calling coda_close(), working around upstream coda_rinex_close() omitting
// free(product->mem_ptr). Always declare after ScopedCodaSession.
class ScopedRinexProduct {
 public:
  ScopedRinexProduct() = default;
  ~ScopedRinexProduct() { Reset(); }

  ScopedRinexProduct(const ScopedRinexProduct&) = delete;
  ScopedRinexProduct& operator=(const ScopedRinexProduct&) = delete;

  void Reset(coda_product* product = nullptr) {
    if (product_ != nullptr) {
      // TODO(schwehr): Remove once coda_rinex_close() frees product->mem_ptr.
      std::free(product_->mem_ptr);
      product_->mem_ptr = nullptr;
      coda_close(product_);
    }
    product_ = product;
  }

  coda_product** OutParam() {
    Reset();
    return &product_;
  }

  coda_product* get() const { return product_; }

 private:
  coda_product* product_ = nullptr;
};

template <typename Fn>
decltype(auto) CallAllowingUpstreamRinexLeak(Fn&& fn) {
  // Upstream coda_rinex_close() never frees product->mem_ptr, and
  // coda_open() / coda_recognize_file() / handle_observation_definition()
  // leak internal allocations on RINEX files. Keep absl::LeakCheckDisabler
  // unconditional so both Google3's default tcmalloc HeapLeakChecker and LSan
  // suppress these upstream allocations.
  const absl::LeakCheckDisabler leak_check_disabler;
  return std::forward<Fn>(fn)();
}

// Formats an 80-column fixed-width RINEX header line (60-char body + 20-char
// label) followed by `eol`.
std::string HeaderLine(std::string_view body, std::string_view label,
                       std::string_view eol = "\n") {
  std::string line(80, ' ');
  const size_t body_len = std::min<size_t>(body.size(), 60);
  if (body_len > 0) {
    std::copy_n(body.data(), body_len, line.begin());
  }
  const size_t label_len = std::min<size_t>(label.size(), 20);
  if (label_len > 0) {
    std::copy_n(label.data(), label_len, line.begin() + 60);
  }
  line.append(eol);
  return line;
}

std::string ReadCursorString(const coda_cursor& cursor) {
  long length = -1;
  if (coda_cursor_get_string_length(&cursor, &length) != CODA_SUCCESS ||
      length < 0) {
    ADD_FAILURE() << "coda_cursor_get_string_length failed: "
                  << coda_errno_to_string(coda_errno);
    return {};
  }
  std::string out(static_cast<size_t>(length) + 1, '\0');
  if (coda_cursor_read_string(&cursor, out.data(), length + 1) !=
      CODA_SUCCESS) {
    ADD_FAILURE() << "coda_cursor_read_string failed: "
                  << coda_errno_to_string(coda_errno);
    return {};
  }
  out.resize(static_cast<size_t>(length));
  return out;
}

template <typename T>
T ReadField(const coda_cursor& parent, const char* path,
            int (*reader)(const coda_cursor*, T*)) {
  coda_cursor cursor = parent;
  if (coda_cursor_goto(&cursor, path) != CODA_SUCCESS) {
    ADD_FAILURE() << "coda_cursor_goto(" << path
                  << ") failed: " << coda_errno_to_string(coda_errno);
    return T{};
  }
  T value{};
  if (reader(&cursor, &value) != CODA_SUCCESS) {
    ADD_FAILURE() << "reader at " << path
                  << " failed: " << coda_errno_to_string(coda_errno);
    return T{};
  }
  return value;
}

std::string ReadFieldString(const coda_cursor& parent, const char* path) {
  coda_cursor cursor = parent;
  if (coda_cursor_goto(&cursor, path) != CODA_SUCCESS) {
    ADD_FAILURE() << "coda_cursor_goto(" << path
                  << ") failed: " << coda_errno_to_string(coda_errno);
    return {};
  }
  return ReadCursorString(cursor);
}

int CheckFieldAvailable(const coda_cursor& record_cursor, const char* name) {
  coda_type* record_type = nullptr;
  long index = -1;
  int available = -1;
  if (coda_cursor_get_type(&record_cursor, &record_type) != CODA_SUCCESS ||
      coda_type_get_record_field_index_from_name(record_type, name, &index) !=
          CODA_SUCCESS ||
      coda_cursor_get_record_field_available_status(
          &record_cursor, index, &available) != CODA_SUCCESS) {
    ADD_FAILURE() << "CheckFieldAvailable(" << name
                  << ") failed: " << coda_errno_to_string(coda_errno);
    return -1;
  }
  return available;
}

// Builds a minimal valid RINEX v3.00 Observation header with one GPS
// observable (C1C).
std::string MakeMinimalObsV3Header(char sat_sys = 'G',
                                   std::string_view eol = "\n") {
  std::string out;
  out += HeaderLine(
      absl::StrFormat("     3.00           O                   %c", sat_sys),
      "RINEX VERSION / TYPE", eol);
  out +=
      HeaderLine("CODATEST            STCORP              20200101 123045 UTC",
                 "PGM / RUN BY / DATE", eol);
  out += HeaderLine("TEST_MARKER", "MARKER NAME", eol);
  out += HeaderLine("OBSERVER            AGENCY", "OBSERVER / AGENCY", eol);
  out += HeaderLine("REC001              TEST_RCV            1.0",
                    "REC # / TYPE / VERS", eol);
  out += HeaderLine("ANT001              TEST_ANT", "ANT # / TYPE", eol);
  out += HeaderLine("        1.0000        2.0000        3.0000",
                    "ANTENNA: DELTA H/E/N", eol);
  out += HeaderLine("G    1 C1C", "SYS / # / OBS TYPES", eol);
  out += HeaderLine("  2020    01    01    00    00   00.0000000     GPS",
                    "TIME OF FIRST OBS", eol);
  out += HeaderLine("", "END OF HEADER", eol);
  return out;
}

// Builds a minimal valid RINEX v3.00 Navigation header.
std::string MakeMinimalNavV3Header(char sat_sys = 'G') {
  std::string out;
  out += HeaderLine(
      absl::StrFormat("     3.00           N                   %c", sat_sys),
      "RINEX VERSION / TYPE");
  out +=
      HeaderLine("CODATEST            STCORP              20200101 123045 UTC",
                 "PGM / RUN BY / DATE");
  out += HeaderLine("", "END OF HEADER");
  return out;
}

// Builds a minimal valid RINEX Clock header (v2.00 or v3.00).
std::string MakeMinimalClockHeader(std::string_view version = "     3.00",
                                   char sat_sys = 'G') {
  std::string out;
  out += HeaderLine(
      absl::StrFormat("%s           C                   %c", version, sat_sys),
      "RINEX VERSION / TYPE");
  out +=
      HeaderLine("CODATEST            STCORP              20200101 123045 UTC",
                 "PGM / RUN BY / DATE");
  out += HeaderLine("", "END OF HEADER");
  return out;
}

// Formats one RINEX Clock record line 0 (and optional continuation line 1 for
// num_values > 2).
std::string MakeClockRecord(std::string_view clk_type, std::string_view name,
                            std::string_view epoch, int num_values,
                            std::string_view bias,
                            std::string_view bias_sigma = "",
                            std::string_view cont_line = "") {
  std::string line(40, ' ');
  const size_t clk_len = std::min<size_t>(clk_type.size(), 2);
  if (clk_len > 0) {
    std::copy_n(clk_type.data(), clk_len, line.begin());
  }
  const size_t name_len = std::min<size_t>(name.size(), 4);
  if (name_len > 0) {
    std::copy_n(name.data(), name_len, line.begin() + 3);
  }
  const size_t epoch_len = std::min<size_t>(epoch.size(), 26);
  if (epoch_len > 0) {
    std::copy_n(epoch.data(), epoch_len, line.begin() + 8);
  }
  const std::string count = absl::StrFormat("%3d", num_values);
  std::copy_n(count.data(), 3, line.begin() + 34);
  line += bias;
  if (!bias_sigma.empty()) {
    line.push_back(' ');
    line += bias_sigma;
  }
  line.push_back('\n');
  if (!cont_line.empty()) {
    line += cont_line;
    line.push_back('\n');
  }
  return line;
}

// Formats a 19-column fixed-width RINEX floating-point value.
std::string NavVal(double v) { return absl::StrFormat("%19.12E", v); }

// Builds one complete RINEX v3.00 Navigation record for `sat_sys` ('G', 'R',
// 'E', or 'S') with `num_values` values starting at `base_val`.
std::string MakeNavRecord(char sat_sys, int sat_num, int num_values,
                          double base_val) {
  std::string out =
      absl::StrFormat("%c%02d 2020 01 01 00 00 00", sat_sys, sat_num);
  for (int i = 0; i < num_values; ++i) {
    if ((i + 1) % 4 == 0) {
      out += "\n    ";
    }
    // Mix Fortran 'D' and standard 'E' exponents to exercise both paths in
    // coda_ascii_parse_double().
    std::string field = NavVal(base_val + i);
    if (i % 2 == 0) {
      std::replace(field.begin(), field.end(), 'E', 'D');
    }
    out += field;
  }
  out.push_back('\n');
  return out;
}

TEST(CodaRinexTest, DoneWithoutInitRinexAndRepeatedSessionCleanup) {
  ScopedRinexTestEnv env;

  // 1. coda_done() when rinex_type == NULL (no RINEX file opened) safely
  //    executes coda_rinex_done() early return.
  {
    ScopedCodaSession session;
    ASSERT_EQ(CODA_SUCCESS, session.Init());
  }

  // 2. Opening RINEX files across two independent coda_init/coda_done sessions
  //    exercises rinex_init() both cold and warm, and coda_rinex_done()
  //    cleanup.
  TempFile clk_file("rinex_session_reuse.clk");
  ASSERT_TRUE(clk_file.WriteText(MakeMinimalClockHeader()));

  for (int pass = 0; pass < 2; ++pass) {
    SCOPED_TRACE(pass);
    ScopedCodaSession session;
    ASSERT_EQ(CODA_SUCCESS, session.Init());
    ScopedRinexProduct p1;
    ScopedRinexProduct p2;
    ASSERT_EQ(CODA_SUCCESS, CallAllowingUpstreamRinexLeak([&] {
                return coda_open(clk_file.c_str(), p1.OutParam());
              }));
    ASSERT_EQ(CODA_SUCCESS, CallAllowingUpstreamRinexLeak([&] {
                return coda_open(clk_file.c_str(), p2.OutParam());
              }));
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, p1.get()));
    EXPECT_FLOAT_EQ(3.0F, ReadField(cursor, "/header/format_version",
                                    coda_cursor_read_float));
  }
}

TEST(CodaRinexTest, RecognizeFileDetectsRinexVersionsAndTypes) {
  ScopedRinexTestEnv env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  struct RecognizeCase {
    std::string_view name;
    std::string content;
  };
  const RecognizeCase kValidCases[] = {
      {.name = "obs_v3.rnx", .content = MakeMinimalObsV3Header('G')},
      {.name = "nav_v3.rnx", .content = MakeMinimalNavV3Header('G')},
      {.name = "clk_v2.rnx",
       .content = MakeMinimalClockHeader("     2.00", ' ')},
      {.name = "clk_v3.rnx",
       .content = MakeMinimalClockHeader("     3.00", 'M')},
  };

  for (const RecognizeCase& tc : kValidCases) {
    SCOPED_TRACE(tc.name);
    TempFile file(tc.name);
    ASSERT_TRUE(file.WriteText(tc.content));
    int64_t size = -1;
    coda_format fmt = coda_format_binary;
    const char* pclass = "sentinel";
    const char* ptype = "sentinel";
    int version = 99;
    ASSERT_EQ(CODA_SUCCESS, CallAllowingUpstreamRinexLeak([&] {
                return coda_recognize_file(file.c_str(), &size, &fmt, &pclass,
                                           &ptype, &version);
              }));
    EXPECT_EQ(static_cast<int64_t>(tc.content.size()), size);
    EXPECT_EQ(coda_format_rinex, fmt);
    EXPECT_EQ(nullptr, pclass);
    EXPECT_EQ(nullptr, ptype);
    EXPECT_EQ(-1, version);
  }

  // Unsupported RINEX types (Meteorological 'M' and v2.x Obs/Nav) still match
  // get_format() as RINEX and fail in read_main_header().
  const RecognizeCase kUnsupportedCases[] = {
      {.name = "met_v2.rnx",
       .content = HeaderLine("     2.11           M", "RINEX VERSION / TYPE")},
      {.name = "met_v3.rnx",
       .content = HeaderLine("     3.00           M", "RINEX VERSION / TYPE")},
      {.name = "obs_v2.rnx",
       .content = HeaderLine("     2.11           O", "RINEX VERSION / TYPE")},
      {.name = "nav_v2.rnx",
       .content = HeaderLine("     2.11           N", "RINEX VERSION / TYPE")},
  };
  for (const RecognizeCase& tc : kUnsupportedCases) {
    SCOPED_TRACE(tc.name);
    TempFile file(tc.name);
    ASSERT_TRUE(file.WriteText(tc.content));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, CallAllowingUpstreamRinexLeak([&] {
      return coda_recognize_file(file.c_str(), nullptr, nullptr, nullptr,
                                 nullptr, nullptr);
    }));
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
  }
}

TEST(CodaRinexTest, OpenObservationV3FullHeaderContinuationAndEpochs) {
  ScopedRinexTestEnv env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // Use CRLF line endings throughout to verify get_line() strips both \n and
  // \r.
  constexpr std::string_view kCrLf = "\r\n";
  std::string content;
  content += HeaderLine("     3.00           O                   M",
                        "RINEX VERSION / TYPE", kCrLf);
  content +=
      HeaderLine("CODATEST            STCORP              20200101 123045 UTC",
                 "PGM / RUN BY / DATE", kCrLf);
  content += HeaderLine("Observation test comment line", "COMMENT", kCrLf);
  content += HeaderLine("TEST_MARKER_60", "MARKER NAME", kCrLf);
  content += HeaderLine("M001", "MARKER NUMBER", kCrLf);
  content += HeaderLine("GEODETIC", "MARKER TYPE", kCrLf);
  content +=
      HeaderLine("J. DOE              TEST_AGENCY", "OBSERVER / AGENCY", kCrLf);
  content += HeaderLine("12345               TRIMBLE             5.42",
                        "REC # / TYPE / VERS", kCrLf);
  content +=
      HeaderLine("67890               TRM59800.00", "ANT # / TYPE", kCrLf);
  content += HeaderLine("  1234567.5000  2345678.2500  3456789.7500",
                        "APPROX POSITION XYZ", kCrLf);
  content += HeaderLine("        1.5000        0.2500       -0.1250",
                        "ANTENNA: DELTA H/E/N", kCrLf);
  // Ignored RINEX v3 observation header records.
  content += HeaderLine("        0.0000        0.0000        0.0000",
                        "ANTENNA: DELTA X/Y/Z", kCrLf);
  content += HeaderLine("G L1    0.0000", "ANTENNA: PHASECENTER", kCrLf);
  content += HeaderLine("        0.0000        0.0000        1.0000",
                        "ANTENNA: B.SIGHT XYZ", kCrLf);
  content += HeaderLine("        0.0000", "ANTENNA: ZERODIR AZI", kCrLf);
  content += HeaderLine("        0.0000        0.0000        0.0000",
                        "ANTENNA: ZERODIR XYZ", kCrLf);
  content += HeaderLine("        0.0000        0.0000        0.0000",
                        "CENTER OF MASS: XYZ", kCrLf);
  content += HeaderLine("G TEST_DCB", "SYS / DCBS APPLIED", kCrLf);
  content += HeaderLine("G TEST_PCV", "SYS / PCVS APPLIED", kCrLf);
  content += HeaderLine("G    1", "SYS / SCALE FACTOR", kCrLf);
  content += HeaderLine("   G01     1", "PRN / # OF OBS", kCrLf);

  // GPS with 15 observables -> 13 on line 1 + 2 on continuation line.
  content +=
      HeaderLine("G   15 C1C L1C D1C S1C C2W L2W D2W S2W C2X L2X D2X S2X C5Q",
                 "SYS / # / OBS TYPES", kCrLf);
  content += HeaderLine("       L5Q S5Q", "SYS / # / OBS TYPES", kCrLf);
  // GLONASS with 27 observables -> 13 + 13 + 1 across two continuation lines.
  content +=
      HeaderLine("R   27 C1C L1C D1C S1C C1P L1P D1P S1P C2C L2C D2C S2C C2P",
                 "SYS / # / OBS TYPES", kCrLf);
  content +=
      HeaderLine("       L2P D2P S2P C3I L3I D3I S3I C4A L4A D4A S4A C6A L6A",
                 "SYS / # / OBS TYPES", kCrLf);
  content += HeaderLine("       S6A", "SYS / # / OBS TYPES", kCrLf);
  // Galileo and SBAS with 3 and 2 observables.
  content += HeaderLine("E    3 C1C L1C S1C", "SYS / # / OBS TYPES", kCrLf);
  content += HeaderLine("S    2 C1C L1C", "SYS / # / OBS TYPES", kCrLf);

  content += HeaderLine("DBHZ", "SIGNAL STRENGTH UNIT", kCrLf);
  content += HeaderLine("    30.000", "INTERVAL", kCrLf);
  content += HeaderLine("  2020     1     1     0     0    0.0000000     GPS",
                        "TIME OF FIRST OBS", kCrLf);
  content += HeaderLine("  2020     1     1     0     5    0.0000000     GPS",
                        "TIME OF LAST OBS", kCrLf);
  content += HeaderLine("     1", "RCV CLOCK OFFS APPL", kCrLf);
  content += HeaderLine("    18", "LEAP SECONDS", kCrLf);
  content += HeaderLine("     4", "# OF SATELLITES", kCrLf);
  content += HeaderLine("", "END OF HEADER", kCrLf);

  // Epoch 0: flag '0', 4 satellites, receiver clock offset present (>= 56).
  content += "> 2020 01 01 00 00  0.0000000  0  4       0.000001250000\r\n";
  // G01: obs 0 has value/LLI=1/sig=7; obs 1 is blank with LLI=2/sig=5; obs
  // 2..13 are present; obs 14 omits trailing LLI/sig (length = 3 + 15*16 - 2)
  // to test trailing-blank padding.
  std::string g01 = "G01";
  g01 += "  20000000.12317";
  g01 += "              25";
  for (int i = 2; i < 14; ++i) {
    g01 += absl::StrFormat("  %12.3f  ", 100.0 + i);
  }
  g01 += "      1234.500";  // 14 chars without LLI/sig -> length = 241.
  content += g01 + "\r\n";

  // R02: 27 observables.
  std::string r02 = "R02";
  for (int i = 0; i < 27; ++i) {
    r02 += absl::StrFormat("  %12.3f09", 200.0 + i);
  }
  content += r02 + "\r\n";

  // E03: 3 observables (16 chars each).
  content += "E03  21000000.50038 110000000.25006        45.000 4\r\n";
  // S20: 2 observables (16 chars each).
  content += "S20  36000000.00005 180000000.00000\r\n";

  // Epoch 1: blank epoch (nan), flag '2' (event), 2 skipped lines, no clock
  // offset (< 56 chars).
  content += ">                              2  2\r\n";
  content += "EVENT LINE ONE (IGNORED BY EPOCH FLAG != 0)\r\n";
  content += "EVENT LINE TWO (IGNORED BY EPOCH FLAG != 0)\r\n";

  TempFile obs_file("rinex_obs_v3_full.rnx");
  ASSERT_TRUE(obs_file.WriteText(content));

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    ScopedRinexProduct product;
    ASSERT_EQ(CODA_SUCCESS, CallAllowingUpstreamRinexLeak([&] {
                return coda_open(obs_file.c_str(), product.OutParam());
              }));

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_rinex_cursor_set_product(&cursor, product.get()));

    // 1. Verify header fields.
    EXPECT_FLOAT_EQ(3.0F, ReadField(cursor, "/header/format_version",
                                    coda_cursor_read_float));
    EXPECT_EQ('O',
              ReadField(cursor, "/header/file_type", coda_cursor_read_char));
    EXPECT_EQ('M', ReadField(cursor, "/header/satellite_system",
                             coda_cursor_read_char));
    EXPECT_EQ("CODATEST", ReadFieldString(cursor, "/header/program"));
    EXPECT_EQ("STCORP", ReadFieldString(cursor, "/header/run_by"));
    EXPECT_EQ("UTC", ReadFieldString(cursor, "/header/datetime_time_zone"));
    EXPECT_EQ("TEST_MARKER_60", ReadFieldString(cursor, "/header/marker_name"));
    EXPECT_EQ("M001", ReadFieldString(cursor, "/header/marker_number"));
    EXPECT_EQ("GEODETIC", ReadFieldString(cursor, "/header/marker_type"));
    EXPECT_EQ("J. DOE", ReadFieldString(cursor, "/header/observer"));
    EXPECT_EQ("TEST_AGENCY", ReadFieldString(cursor, "/header/agency"));
    EXPECT_EQ("12345", ReadFieldString(cursor, "/header/receiver_number"));
    EXPECT_EQ("TRIMBLE", ReadFieldString(cursor, "/header/receiver_type"));
    EXPECT_EQ("5.42", ReadFieldString(cursor, "/header/receiver_version"));
    EXPECT_EQ("67890", ReadFieldString(cursor, "/header/antenna_number"));
    EXPECT_EQ("TRM59800.00", ReadFieldString(cursor, "/header/antenna_type"));
    EXPECT_FLOAT_EQ(1234567.5F, ReadField(cursor, "/header/approx_position_x",
                                          coda_cursor_read_float));
    EXPECT_FLOAT_EQ(2345678.25F, ReadField(cursor, "/header/approx_position_y",
                                           coda_cursor_read_float));
    EXPECT_FLOAT_EQ(3456789.75F, ReadField(cursor, "/header/approx_position_z",
                                           coda_cursor_read_float));
    EXPECT_FLOAT_EQ(1.5F, ReadField(cursor, "/header/antenna_delta_h",
                                    coda_cursor_read_float));
    EXPECT_FLOAT_EQ(0.25F, ReadField(cursor, "/header/antenna_delta_e",
                                     coda_cursor_read_float));
    EXPECT_FLOAT_EQ(-0.125F, ReadField(cursor, "/header/antenna_delta_n",
                                       coda_cursor_read_float));
    EXPECT_EQ("DBHZ", ReadFieldString(cursor, "/header/signal_strength_unit"));
    EXPECT_DOUBLE_EQ(30.0, ReadField(cursor, "/header/obs_interval",
                                     coda_cursor_read_double));
    EXPECT_EQ("GPS",
              ReadFieldString(cursor, "/header/time_of_first_obs_time_zone"));
    EXPECT_EQ("GPS",
              ReadFieldString(cursor, "/header/time_of_last_obs_time_zone"));
    EXPECT_EQ(1, ReadField(cursor, "/header/rcv_clock_offs_appl",
                           coda_cursor_read_uint8));
    EXPECT_EQ(
        18, ReadField(cursor, "/header/leap_seconds", coda_cursor_read_int32));
    EXPECT_EQ(4, ReadField(cursor, "/header/num_satellites",
                           coda_cursor_read_uint16));

    double expected_dt = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_time_parts_to_double(2020, 1, 1, 12, 30, 45, 0,
                                                      &expected_dt));
    EXPECT_DOUBLE_EQ(expected_dt, ReadField(cursor, "/header/datetime",
                                            coda_cursor_read_double));

    double expected_first_obs = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_time_parts_to_double(2020, 1, 1, 0, 0, 0, 0,
                                                      &expected_first_obs));
    EXPECT_DOUBLE_EQ(expected_first_obs,
                     ReadField(cursor, "/header/time_of_first_obs",
                               coda_cursor_read_double));

    // 2. Verify /header/sys descriptors across continuation lines.
    EXPECT_EQ('G',
              ReadField(cursor, "/header/sys[0]/code", coda_cursor_read_char));
    EXPECT_EQ(15, ReadField(cursor, "/header/sys[0]/num_obs_types",
                            coda_cursor_read_int16));
    EXPECT_EQ("C1C", ReadFieldString(cursor, "/header/sys[0]/descriptor[0]"));
    EXPECT_EQ("S5Q", ReadFieldString(cursor, "/header/sys[0]/descriptor[14]"));
    EXPECT_EQ('R',
              ReadField(cursor, "/header/sys[1]/code", coda_cursor_read_char));
    EXPECT_EQ(27, ReadField(cursor, "/header/sys[1]/num_obs_types",
                            coda_cursor_read_int16));
    EXPECT_EQ("S6A", ReadFieldString(cursor, "/header/sys[1]/descriptor[26]"));

    // 3. Verify Epoch 0 observations across all 4 constellations.
    EXPECT_EQ('0', ReadField(cursor, "/record[0]/flag", coda_cursor_read_char));
    EXPECT_NEAR(1.25e-6,
                ReadField(cursor, "/record[0]/receiver_clock_offset",
                          coda_cursor_read_double),
                1e-12);
    EXPECT_EQ(1, ReadField(cursor, "/record[0]/gps[0]/number",
                           coda_cursor_read_uint8));
    EXPECT_DOUBLE_EQ(20000000.123,
                     ReadField(cursor, "/record[0]/gps[0]/C1C/observation",
                               coda_cursor_read_double));
    EXPECT_EQ(1, ReadField(cursor, "/record[0]/gps[0]/C1C/lli",
                           coda_cursor_read_uint8));
    EXPECT_EQ(7, ReadField(cursor, "/record[0]/gps[0]/C1C/signal_strength",
                           coda_cursor_read_uint8));
    EXPECT_DOUBLE_EQ(0.0, ReadField(cursor, "/record[0]/gps[0]/L1C/observation",
                                    coda_cursor_read_double));
    EXPECT_EQ(2, ReadField(cursor, "/record[0]/gps[0]/L1C/lli",
                           coda_cursor_read_uint8));
    EXPECT_EQ(5, ReadField(cursor, "/record[0]/gps[0]/L1C/signal_strength",
                           coda_cursor_read_uint8));
    EXPECT_DOUBLE_EQ(1234.5,
                     ReadField(cursor, "/record[0]/gps[0]/S5Q/observation",
                               coda_cursor_read_double));
    EXPECT_EQ(0, ReadField(cursor, "/record[0]/gps[0]/S5Q/lli",
                           coda_cursor_read_uint8));
    EXPECT_EQ(0, ReadField(cursor, "/record[0]/gps[0]/S5Q/signal_strength",
                           coda_cursor_read_uint8));

    EXPECT_EQ(2, ReadField(cursor, "/record[0]/glonass[0]/number",
                           coda_cursor_read_uint8));
    EXPECT_DOUBLE_EQ(226.0,
                     ReadField(cursor, "/record[0]/glonass[0]/S6A/observation",
                               coda_cursor_read_double));
    EXPECT_EQ(9, ReadField(cursor, "/record[0]/glonass[0]/S6A/signal_strength",
                           coda_cursor_read_uint8));

    EXPECT_EQ(3, ReadField(cursor, "/record[0]/galileo[0]/number",
                           coda_cursor_read_uint8));
    EXPECT_DOUBLE_EQ(21000000.5,
                     ReadField(cursor, "/record[0]/galileo[0]/C1C/observation",
                               coda_cursor_read_double));

    EXPECT_EQ(20, ReadField(cursor, "/record[0]/sbas[0]/number",
                            coda_cursor_read_uint8));
    EXPECT_DOUBLE_EQ(36000000.0,
                     ReadField(cursor, "/record[0]/sbas[0]/C1C/observation",
                               coda_cursor_read_double));

    // 4. Verify Epoch 1 (event flag != '0', blank epoch -> NaN, clock == 0.0).
    EXPECT_EQ('2', ReadField(cursor, "/record[1]/flag", coda_cursor_read_char));
    EXPECT_TRUE(std::isnan(
        ReadField(cursor, "/record[1]/epoch", coda_cursor_read_double)));
    EXPECT_DOUBLE_EQ(0.0, ReadField(cursor, "/record[1]/receiver_clock_offset",
                                    coda_cursor_read_double));
    coda_cursor gps_arr = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&gps_arr, "/record[1]/gps"));
    long num_gps = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&gps_arr, &num_gps));
    EXPECT_EQ(0L, num_gps);
  }
}

TEST(CodaRinexTest, OpenObservationTimeSystemDefaultsAndOptionalFields) {
  ScopedRinexTestEnv env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  struct SysDefaultCase {
    char sat_sys;
    std::string_view expected_tz;
  };
  constexpr SysDefaultCase kCases[] = {
      {.sat_sys = 'G', .expected_tz = "GPS"},
      {.sat_sys = 'R', .expected_tz = "GLO"},
      {.sat_sys = 'E', .expected_tz = "GAL"},
  };

  for (const SysDefaultCase& tc : kCases) {
    SCOPED_TRACE(tc.expected_tz);
    std::string content;
    content +=
        HeaderLine(absl::StrFormat("     3.00           O                   %c",
                                   tc.sat_sys),
                   "RINEX VERSION / TYPE");
    // Blank datetime (15 spaces) -> NaN on read.
    content += HeaderLine("CODATEST            STCORP", "PGM / RUN BY / DATE");
    content += HeaderLine("M_NO_OPT", "MARKER NAME");
    content += HeaderLine("OBS                 AG", "OBSERVER / AGENCY");
    content += HeaderLine("R1                  T1                  V1",
                          "REC # / TYPE / VERS");
    content += HeaderLine("A1                  T1", "ANT # / TYPE");
    content += HeaderLine("        0.0000        0.0000        0.0000",
                          "ANTENNA: DELTA H/E/N");
    content += HeaderLine(absl::StrFormat("%c    1 C1C", tc.sat_sys),
                          "SYS / # / OBS TYPES");
    // Blank time system at cols 48..50 triggers default from satellite_system.
    content += HeaderLine("  2020    01    01    00    00   00.0000000",
                          "TIME OF FIRST OBS");
    content += HeaderLine("  2020    01    01    00    01   00.0000000",
                          "TIME OF LAST OBS");
    // Omit trailing newline on END OF HEADER to cover non-newline EOF path.
    std::string end_line = HeaderLine("", "END OF HEADER");
    end_line.pop_back();
    content += end_line;

    TempFile file(absl::StrFormat("obs_default_%c.rnx", tc.sat_sys));
    ASSERT_TRUE(file.WriteText(content));

    ScopedRinexProduct product;
    ASSERT_EQ(CODA_SUCCESS, CallAllowingUpstreamRinexLeak([&] {
                return coda_open(file.c_str(), product.OutParam());
              }));

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    EXPECT_EQ(tc.expected_tz,
              ReadFieldString(cursor, "/header/time_of_first_obs_time_zone"));
    EXPECT_EQ(tc.expected_tz,
              ReadFieldString(cursor, "/header/time_of_last_obs_time_zone"));
    EXPECT_TRUE(std::isnan(
        ReadField(cursor, "/header/datetime", coda_cursor_read_double)));

    coda_cursor hdr = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&hdr, "/header"));
    EXPECT_EQ(0, CheckFieldAvailable(hdr, "marker_number"));
    EXPECT_EQ(0, CheckFieldAvailable(hdr, "marker_type"));
    EXPECT_EQ(0, CheckFieldAvailable(hdr, "approx_position_x"));
    EXPECT_EQ(0, CheckFieldAvailable(hdr, "signal_strength_unit"));
    EXPECT_EQ(0, CheckFieldAvailable(hdr, "obs_interval"));
    EXPECT_EQ(0, CheckFieldAvailable(hdr, "rcv_clock_offs_appl"));
    EXPECT_EQ(0, CheckFieldAvailable(hdr, "leap_seconds"));
    EXPECT_EQ(0, CheckFieldAvailable(hdr, "num_satellites"));
  }
}

TEST(CodaRinexTest, OpenNavigationV3HeaderAndAllConstellations) {
  ScopedRinexTestEnv env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  std::string content;
  content += HeaderLine("     3.00           N                   M",
                        "RINEX VERSION / TYPE");
  content +=
      HeaderLine("CODANAV             STCORP              20200101 000000 UTC",
                 "PGM / RUN BY / DATE");
  content += HeaderLine("Navigation header comment", "COMMENT");
  content += HeaderLine("GPSA  1.0000D-08  2.0000D-08 -3.0000D-08  4.0000D-08",
                        "IONOSPHERIC CORR");
  content += HeaderLine("GAL   5.0000E+01  1.0000E-01  0.0000E+00  0.0000E+00",
                        "IONOSPHERIC CORR");
  content += HeaderLine("GPUT  1.2500000000D-09 2.500000000D-16 147456 2086",
                        "TIME SYSTEM CORR");
  content +=
      HeaderLine("SBUT  3.0000000000D-09 4.000000000D-16  86400 2086 EGNOS  5",
                 "TIME SYSTEM CORR");
  content += HeaderLine("    18", "LEAP SECONDS");
  content += HeaderLine("", "END OF HEADER");

  // One record each for GPS (29 values), GLONASS (15), Galileo (28), SBAS (15).
  content += MakeNavRecord('G', 1, 29, 1.0);
  content += MakeNavRecord('R', 2, 15, 100.0);
  content += MakeNavRecord('E', 3, 28, 200.0);
  content += MakeNavRecord('S', 20, 15, 300.0);

  TempFile nav_file("rinex_nav_v3_all.rnx");
  ASSERT_TRUE(nav_file.WriteText(content));

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    ScopedRinexProduct product;
    ASSERT_EQ(CODA_SUCCESS, CallAllowingUpstreamRinexLeak([&] {
                return coda_open(nav_file.c_str(), product.OutParam());
              }));

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

    // 1. Header fields & corrections.
    EXPECT_FLOAT_EQ(3.0F, ReadField(cursor, "/header/format_version",
                                    coda_cursor_read_float));
    EXPECT_EQ('N',
              ReadField(cursor, "/header/file_type", coda_cursor_read_char));
    EXPECT_EQ('M', ReadField(cursor, "/header/satellite_system",
                             coda_cursor_read_char));
    EXPECT_EQ("CODANAV", ReadFieldString(cursor, "/header/program"));
    EXPECT_EQ("STCORP", ReadFieldString(cursor, "/header/run_by"));
    EXPECT_EQ("UTC", ReadFieldString(cursor, "/header/datetime_time_zone"));
    double expected_dt = 0.0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_time_parts_to_double(2020, 1, 1, 0, 0, 0, 0, &expected_dt));
    EXPECT_DOUBLE_EQ(expected_dt, ReadField(cursor, "/header/datetime",
                                            coda_cursor_read_double));
    EXPECT_EQ(
        18, ReadField(cursor, "/header/leap_seconds", coda_cursor_read_int32));

    EXPECT_EQ("GPSA",
              ReadFieldString(cursor, "/header/ionospheric_corr[0]/type"));
    EXPECT_NEAR(-3.0e-8,
                ReadField(cursor, "/header/ionospheric_corr[0]/parameter[2]",
                          coda_cursor_read_double),
                1e-18);
    EXPECT_EQ("GAL",
              ReadFieldString(cursor, "/header/ionospheric_corr[1]/type"));
    EXPECT_DOUBLE_EQ(
        50.0, ReadField(cursor, "/header/ionospheric_corr[1]/parameter[0]",
                        coda_cursor_read_double));

    EXPECT_EQ("GPUT",
              ReadFieldString(cursor, "/header/time_system_corr[0]/type"));
    EXPECT_NEAR(1.25e-9,
                ReadField(cursor, "/header/time_system_corr[0]/a0",
                          coda_cursor_read_double),
                1e-18);
    EXPECT_EQ(147456, ReadField(cursor, "/header/time_system_corr[0]/T",
                                coda_cursor_read_int32));
    EXPECT_EQ(2086, ReadField(cursor, "/header/time_system_corr[0]/W",
                              coda_cursor_read_int16));
    coda_cursor gput_cursor = cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto(&gput_cursor, "/header/time_system_corr[0]"));
    EXPECT_EQ(0, CheckFieldAvailable(gput_cursor, "S"));
    EXPECT_EQ(0, CheckFieldAvailable(gput_cursor, "U"));

    EXPECT_EQ("SBUT",
              ReadFieldString(cursor, "/header/time_system_corr[1]/type"));
    EXPECT_EQ("EGNOS",
              ReadFieldString(cursor, "/header/time_system_corr[1]/S"));
    EXPECT_EQ(5, ReadField(cursor, "/header/time_system_corr[1]/U",
                           coda_cursor_read_uint8));

    // 2. GPS navigation record.
    EXPECT_EQ(1, ReadField(cursor, "/gps[0]/number", coda_cursor_read_uint8));
    EXPECT_DOUBLE_EQ(1.0, ReadField(cursor, "/gps[0]/sv_clock_bias",
                                    coda_cursor_read_double));
    EXPECT_DOUBLE_EQ(29.0, ReadField(cursor, "/gps[0]/fit_interval",
                                     coda_cursor_read_double));

    // 3. GLONASS navigation record.
    EXPECT_EQ(2,
              ReadField(cursor, "/glonass[0]/number", coda_cursor_read_uint8));
    EXPECT_DOUBLE_EQ(100.0, ReadField(cursor, "/glonass[0]/sv_clock_bias",
                                      coda_cursor_read_double));
    EXPECT_DOUBLE_EQ(114.0, ReadField(cursor, "/glonass[0]/age_of_oper_info",
                                      coda_cursor_read_double));

    // 4. Galileo navigation record (note value index 22 is spare in RINEX 3).
    EXPECT_EQ(3,
              ReadField(cursor, "/galileo[0]/number", coda_cursor_read_uint8));
    EXPECT_EQ(220U, ReadField(cursor, "/galileo[0]/data_sources",
                              coda_cursor_read_uint32));
    EXPECT_DOUBLE_EQ(
        223.0, ReadField(cursor, "/galileo[0]/sisa", coda_cursor_read_double));
    EXPECT_EQ(224U, ReadField(cursor, "/galileo[0]/sv_health",
                              coda_cursor_read_uint32));
    EXPECT_DOUBLE_EQ(227.0, ReadField(cursor, "/galileo[0]/transmission_time",
                                      coda_cursor_read_double));

    // 5. SBAS navigation record.
    EXPECT_EQ(20, ReadField(cursor, "/sbas[0]/number", coda_cursor_read_uint8));
    EXPECT_DOUBLE_EQ(302.0, ReadField(cursor, "/sbas[0]/transmission_time",
                                      coda_cursor_read_double));
    EXPECT_DOUBLE_EQ(
        314.0, ReadField(cursor, "/sbas[0]/iodn", coda_cursor_read_double));
  }
}

TEST(CodaRinexTest, OpenClockV2AndV3HeadersAndAllValueCounts) {
  ScopedRinexTestEnv env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. RINEX v2.00 Clock file exercises satellite_system fallback to 'G',
  //    all ignored clock header keywords, and records with 1..6 values.
  {
    std::string clk2;
    clk2 += HeaderLine("     2.00           C", "RINEX VERSION / TYPE");
    clk2 += HeaderLine(
        "CLKPROG             STCORP              20200101 000000 UTC",
        "PGM / RUN BY / DATE");
    clk2 += HeaderLine("Clock header comment", "COMMENT");
    clk2 += HeaderLine("   GPS", "TIME SYSTEM ID");
    clk2 += HeaderLine("    18", "LEAP SECONDS");
    clk2 += HeaderLine("G DCB", "SYS / DCBS APPLIED");
    clk2 += HeaderLine("G PCV", "SYS / PCVS APPLIED");
    clk2 += HeaderLine("     2    AR    AS", "# / TYPES OF DATA");
    clk2 += HeaderLine("ALGO 40104M002", "STATION NAME / NUM");
    clk2 += HeaderLine("INTERNAL", "STATION CLK REF");
    clk2 += HeaderLine("STCORP ANALYSIS", "ANALYSIS CENTER");
    clk2 += HeaderLine("     1", "# OF CLK REF");
    clk2 += HeaderLine("ALGO 40104M002", "ANALYSIS CLK REF");
    clk2 += HeaderLine("     1    IGS14", "# OF SOLN STA / TRF");
    clk2 += HeaderLine("ALGO 40104M002", "SOLN STA NAME / NUM");
    clk2 += HeaderLine("     1", "# OF SOLN SATS");
    clk2 += HeaderLine("G01", "PRN LIST");
    clk2 += HeaderLine("", "END OF HEADER");

    // Record 0: num_values = 1 (bias only).
    clk2 += MakeClockRecord("AR", "ALGO", "2020 01 01 00 00  0.000000", 1,
                            " 1.000000000000E-04");
    // Record 1: num_values = 2 (bias + bias_sigma), blank epoch -> NaN.
    clk2 += MakeClockRecord("AS", "G01", "", 2, " 2.000000000000E-04",
                            " 1.000000000000E-10");
    // Record 2: num_values = 3 (+ rate on continuation line).
    clk2 += MakeClockRecord("CR", "ALGO", "2020 01 01 00 05  0.000000", 3,
                            " 3.000000000000E-04", " 1.100000000000E-10",
                            " 1.000000000000E-11");
    // Record 3: num_values = 4 (+ rate_sigma).
    clk2 += MakeClockRecord("DR", "ALGO", "2020 01 01 00 10  0.000000", 4,
                            " 4.000000000000E-04", " 1.200000000000E-10",
                            " 2.000000000000E-11  3.000000000000E-13");
    // Record 4: num_values = 5 (+ acceleration).
    clk2 += MakeClockRecord(
        "MS", "ALGO", "2020 01 01 00 15  0.000000", 5, " 5.000000000000E-04",
        " 1.300000000000E-10",
        " 3.000000000000E-11  4.000000000000E-13  5.000000000000E-15");
    // Record 5: num_values = 6 (+ acceleration_sigma).
    clk2 += MakeClockRecord(
        "AR", "ALGO", "2020 01 01 00 20  0.000000", 6, " 6.000000000000E-04",
        " 1.400000000000E-10",
        " 4.000000000000E-11  5.000000000000E-13  6.000000000000E-15 "
        " 7.000000000000E-17");

    TempFile file("rinex_clk_v2.clk");
    ASSERT_TRUE(file.WriteText(clk2));
    ScopedRinexProduct product;
    ASSERT_EQ(CODA_SUCCESS, CallAllowingUpstreamRinexLeak([&] {
                return coda_open(file.c_str(), product.OutParam());
              }));

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    EXPECT_FLOAT_EQ(2.0F, ReadField(cursor, "/header/format_version",
                                    coda_cursor_read_float));
    EXPECT_EQ('C',
              ReadField(cursor, "/header/file_type", coda_cursor_read_char));
    EXPECT_EQ('G', ReadField(cursor, "/header/satellite_system",
                             coda_cursor_read_char));
    EXPECT_EQ("GPS", ReadFieldString(cursor, "/header/time_system_id"));
    EXPECT_EQ(
        18, ReadField(cursor, "/header/leap_seconds", coda_cursor_read_int32));

    coda_cursor rec_arr = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&rec_arr, "/record"));
    long num_records = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_num_elements(&rec_arr, &num_records));
    EXPECT_EQ(6L, num_records);

    double expected_r0_epoch = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_time_parts_to_double(2020, 1, 1, 0, 0, 0, 0,
                                                      &expected_r0_epoch));
    EXPECT_DOUBLE_EQ(expected_r0_epoch, ReadField(cursor, "/record[0]/epoch",
                                                  coda_cursor_read_double));
    EXPECT_EQ("AR", ReadFieldString(cursor, "/record[0]/type"));
    EXPECT_EQ("ALGO", ReadFieldString(cursor, "/record[0]/name"));
    EXPECT_DOUBLE_EQ(
        1.0e-4, ReadField(cursor, "/record[0]/bias", coda_cursor_read_double));
    coda_cursor r0 = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&r0, "/record[0]"));
    EXPECT_EQ(0, CheckFieldAvailable(r0, "bias_sigma"));
    EXPECT_EQ(0, CheckFieldAvailable(r0, "rate"));

    EXPECT_TRUE(std::isnan(
        ReadField(cursor, "/record[1]/epoch", coda_cursor_read_double)));
    EXPECT_DOUBLE_EQ(1.0e-10, ReadField(cursor, "/record[1]/bias_sigma",
                                        coda_cursor_read_double));

    EXPECT_DOUBLE_EQ(
        1.0e-11, ReadField(cursor, "/record[2]/rate", coda_cursor_read_double));
    EXPECT_DOUBLE_EQ(3.0e-13, ReadField(cursor, "/record[3]/rate_sigma",
                                        coda_cursor_read_double));
    EXPECT_DOUBLE_EQ(5.0e-15, ReadField(cursor, "/record[4]/acceleration",
                                        coda_cursor_read_double));
    EXPECT_DOUBLE_EQ(7.0e-17, ReadField(cursor, "/record[5]/acceleration_sigma",
                                        coda_cursor_read_double));
  }

  // 2. RINEX v3.00 Clock file with SYS / # / OBS TYPES in header.
  {
    std::string clk3;
    clk3 += HeaderLine("     3.00           C                   M",
                       "RINEX VERSION / TYPE");
    clk3 += HeaderLine(
        "CLK3PROG            STCORP              20200101 000000 UTC",
        "PGM / RUN BY / DATE");
    clk3 += HeaderLine("G    2 C1C L1C", "SYS / # / OBS TYPES");
    clk3 += HeaderLine("", "END OF HEADER");

    TempFile file("rinex_clk_v3.clk");
    ASSERT_TRUE(file.WriteText(clk3));
    ScopedRinexProduct product;
    ASSERT_EQ(CODA_SUCCESS, CallAllowingUpstreamRinexLeak([&] {
                return coda_open(file.c_str(), product.OutParam());
              }));

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    EXPECT_FLOAT_EQ(3.0F, ReadField(cursor, "/header/format_version",
                                    coda_cursor_read_float));
    EXPECT_EQ('M', ReadField(cursor, "/header/satellite_system",
                             coda_cursor_read_char));
    EXPECT_EQ('G',
              ReadField(cursor, "/header/sys[0]/code", coda_cursor_read_char));
    EXPECT_EQ("L1C", ReadFieldString(cursor, "/header/sys[0]/descriptor[1]"));
  }
}

TEST(CodaRinexTest, RejectsUnsupportedVersionsAndTypesIncludingV2ObsNavAndMet) {
  ScopedRinexTestEnv env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  struct MainHeaderErrorCase {
    std::string_view name;
    std::string content;
    int expected_errno;
    std::string_view expected_msg;
  };
  const MainHeaderErrorCase kCases[] = {
      {.name = "obs_v2.rnx",
       .content = HeaderLine("     2.11           O", "RINEX VERSION / TYPE"),
       .expected_errno = CODA_ERROR_UNSUPPORTED_PRODUCT,
       .expected_msg = "Observation data"},
      {.name = "nav_v2.rnx",
       .content = HeaderLine("     2.11           N", "RINEX VERSION / TYPE"),
       .expected_errno = CODA_ERROR_UNSUPPORTED_PRODUCT,
       .expected_msg = "Navigation data"},
      {.name = "clk_v1.rnx",
       .content = HeaderLine("     1.00           C", "RINEX VERSION / TYPE"),
       .expected_errno = CODA_ERROR_UNSUPPORTED_PRODUCT,
       .expected_msg = "Clock data"},
      {.name = "clk_v301.rnx",
       .content = HeaderLine("     3.01           C", "RINEX VERSION / TYPE"),
       .expected_errno = CODA_ERROR_UNSUPPORTED_PRODUCT,
       .expected_msg = "Clock data"},
      {.name = "met_v2.rnx",
       .content = HeaderLine("     2.11           M", "RINEX VERSION / TYPE"),
       .expected_errno = CODA_ERROR_UNSUPPORTED_PRODUCT,
       .expected_msg = "RINEX file type 'M' is not supported"},
      {.name = "met_v3.rnx",
       .content = HeaderLine("     3.00           M", "RINEX VERSION / TYPE"),
       .expected_errno = CODA_ERROR_UNSUPPORTED_PRODUCT,
       .expected_msg = "RINEX file type 'M' is not supported"},
      {.name = "bad_type.rnx",
       .content = HeaderLine("     3.00           Z", "RINEX VERSION / TYPE"),
       .expected_errno = CODA_ERROR_UNSUPPORTED_PRODUCT,
       .expected_msg = "RINEX file type 'Z' is not supported"},
      {.name = "bad_ver.rnx",
       .content = HeaderLine("   BADVER           O", "RINEX VERSION / TYPE"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 1"},
      // Line 1 is < 61 bytes, while file offset 60 still matches
      // "RINEX VERSION / TYPE" for get_format().
      {.name = "short_main_hdr.rnx",
       .content = std::string("short\n") + std::string(54, ' ') +
                  "RINEX VERSION / TYPE\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "header line length (5) too short"},
  };

  for (const MainHeaderErrorCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    TempFile file(tc.name);
    ASSERT_TRUE(file.WriteText(tc.content));
    ScopedRinexProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, CallAllowingUpstreamRinexLeak([&] {
      return coda_open(file.c_str(), product.OutParam());
    }));
    EXPECT_EQ(tc.expected_errno, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr(tc.expected_msg));
    EXPECT_EQ(nullptr, product.get());
  }

  // Direct coda_rinex_reopen() tests for branches unreachable via get_format():
  // 1. Line 1 >= 61 chars without "RINEX VERSION / TYPE" at col 60.
  {
    TempFile file("reopen_wrong_hdr.rnx");
    const std::string line = HeaderLine("     3.00           O", "WRONG LABEL");
    ASSERT_TRUE(file.WriteText(line));
    coda_product* raw_product = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_bin_open(file.c_str(), static_cast<int64_t>(line.size()),
                            &raw_product));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, CallAllowingUpstreamRinexLeak(
                      [&] { return coda_rinex_reopen(&raw_product); }));
    EXPECT_EQ(CODA_ERROR_FILE_READ, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("invalid header item"));
  }

  // 2. File deleted between coda_bin_open() and coda_rinex_reopen() triggers
  //    fopen() failure in read_file().
  {
    TempFile file("reopen_missing_file.rnx");
    const std::string content = MakeMinimalClockHeader();
    ASSERT_TRUE(file.WriteText(content));
    coda_product* raw_product = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_bin_open(file.c_str(), static_cast<int64_t>(content.size()),
                            &raw_product));
    std::filesystem::remove(file.path());
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, CallAllowingUpstreamRinexLeak(
                      [&] { return coda_rinex_reopen(&raw_product); }));
    EXPECT_EQ(CODA_ERROR_FILE_OPEN, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("could not open file"));
  }
}

TEST(CodaRinexTest, RejectsMalformedObservationHeadersAndRecords) {
  ScopedRinexTestEnv env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  struct ErrorCase {
    std::string_view name;
    std::string content;
    int expected_errno;
    std::string_view expected_msg;
  };

  const std::string v3_obs_first = HeaderLine(
      "     3.00           O                   G", "RINEX VERSION / TYPE");
  const std::string base_hdr = MakeMinimalObsV3Header('G');

  const ErrorCase kCases[] = {
      {.name = "short_hdr_line",
       .content = v3_obs_first + "SHORT_LINE\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "header line length (10) too short"},
      {.name = "unknown_hdr_item",
       .content = v3_obs_first + HeaderLine("DATA", "UNKNOWN ITEM"),
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "invalid header item"},
      {.name = "missing_mandatory_field",
       .content = v3_obs_first + HeaderLine("", "END OF HEADER"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory field"},
      {.name = "bad_approx_x",
       .content = v3_obs_first + HeaderLine("BAD_X         1.0           2.0",
                                            "APPROX POSITION XYZ"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "bad_approx_y",
       .content = v3_obs_first + HeaderLine("        1.0000BAD_Y         2.0",
                                            "APPROX POSITION XYZ"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "bad_approx_z",
       .content = v3_obs_first + HeaderLine("        1.0000        2.0000BAD_Z",
                                            "APPROX POSITION XYZ"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "bad_ant_h",
       .content = v3_obs_first + HeaderLine("BAD_H         1.0           2.0",
                                            "ANTENNA: DELTA H/E/N"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "bad_ant_e",
       .content = v3_obs_first + HeaderLine("        1.0000BAD_E         2.0",
                                            "ANTENNA: DELTA H/E/N"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "bad_ant_n",
       .content = v3_obs_first + HeaderLine("        1.0000        2.0000BAD_N",
                                            "ANTENNA: DELTA H/E/N"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "bad_interval",
       .content = v3_obs_first + HeaderLine("BAD_INT", "INTERVAL"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "bad_rcv_clock_appl",
       .content = v3_obs_first + HeaderLine("BAD", "RCV CLOCK OFFS APPL"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "bad_obs_leap_seconds",
       .content = v3_obs_first + HeaderLine("BAD", "LEAP SECONDS"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "bad_num_satellites",
       .content = v3_obs_first + HeaderLine("BAD", "# OF SATELLITES"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "obs_def_bad_sys",
       .content =
           v3_obs_first + HeaderLine("J    1 C1C", "SYS / # / OBS TYPES"),
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "invalid satellite system for observation type"},
      {.name = "obs_def_duplicate_sys",
       .content = v3_obs_first +
                  HeaderLine("G    1 C1C", "SYS / # / OBS TYPES") +
                  HeaderLine("G    1 L1C", "SYS / # / OBS TYPES"),
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "multiple observation type definitions for type 'G'"},
      {.name = "obs_def_bad_count",
       .content =
           v3_obs_first + HeaderLine("G  BAD C1C", "SYS / # / OBS TYPES"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "obs_def_short_continuation",
       .content =
           v3_obs_first +
           HeaderLine(
               "G   15 C1C L1C D1C S1C C2W L2W D2W S2W C2X L2X D2X S2X C5Q",
               "SYS / # / OBS TYPES") +
           "       L5Q\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "header line length (10) too short"},
      {.name = "epoch_line_too_short",
       .content = base_hdr + "> 2020 01 01\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "record line length (12) too short"},
      {.name = "epoch_missing_gt",
       .content = base_hdr + "X 2020 01 01 00 00  0.0000000  0  1\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "expected '>' as start of epoch record"},
      {.name = "epoch_bad_num_sats",
       .content = base_hdr + "> 2020 01 01 00 00  0.0000000  0 XX\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "invalid 'number of satellites' entry"},
      {.name = "epoch_bad_clock_offset",
       .content = base_hdr +
                  "> 2020 01 01 00 00  0.0000000  0  1      BAD_CLKVAL     \n",
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 12"},
      {.name = "sat_bad_sys",
       .content = base_hdr + "> 2020 01 01 00 00  0.0000000  0  1\n" +
                  "X01  20000000.12307\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "invalid satellite system for epoch record"},
      {.name = "sat_undefined_sys",
       .content = base_hdr + "> 2020 01 01 00 00  0.0000000  0  1\n" +
                  "R01  20000000.12307\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "satellite system 'R' was not defined in header"},
      {.name = "sat_line_too_short",
       .content =
           base_hdr + "> 2020 01 01 00 00  0.0000000  0  1\n" + "G01  2000.1\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "epoch line length (11) too short"},
      {.name = "sat_bad_number",
       .content = base_hdr + "> 2020 01 01 00 00  0.0000000  0  1\n" +
                  "GXX  20000000.12307\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "invalid satellite number"},
      {.name = "sat_bad_obs_value",
       .content = base_hdr + "> 2020 01 01 00 00  0.0000000  0  1\n" +
                  "G01    BADVAL    07\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "invalid observation value"},
  };

  for (const ErrorCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    TempFile file(absl::StrCat("bad_obs_", tc.name, ".rnx"));
    ASSERT_TRUE(file.WriteText(tc.content));
    ScopedRinexProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, CallAllowingUpstreamRinexLeak([&] {
      return coda_open(file.c_str(), product.OutParam());
    }));
    EXPECT_EQ(tc.expected_errno, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr(tc.expected_msg));
    EXPECT_EQ(nullptr, product.get());
  }
}

TEST(CodaRinexTest, RejectsMalformedNavigationHeadersAndRecords) {
  ScopedRinexTestEnv env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  struct ErrorCase {
    std::string_view name;
    std::string content;
    int expected_errno;
    std::string_view expected_msg;
  };

  const std::string v3_nav_first = HeaderLine(
      "     3.00           N                   M", "RINEX VERSION / TYPE");
  const std::string base_hdr = MakeMinimalNavV3Header('M');
  const std::string valid_nav_line0 =
      "G01 2020 01 01 00 00 00 1.000000000000D-04 2.000000000000D-12 "
      "0.000000000000D+00\n";

  const ErrorCase kCases[] = {
      {.name = "short_hdr_line",
       .content = v3_nav_first + "SHORT\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "header line length (5) too short"},
      {.name = "unknown_hdr_item",
       .content = v3_nav_first + HeaderLine("DATA", "UNKNOWN NAV ITEM"),
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "invalid header item"},
      {.name = "missing_mandatory_field",
       .content = v3_nav_first + HeaderLine("", "END OF HEADER"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory field"},
      {.name = "bad_iono_param",
       .content =
           v3_nav_first +
           HeaderLine("GPSA BAD_PARAM    2.0D-08      3.0D-08      4.0D-08",
                      "IONOSPHERIC CORR"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "bad_tsc_a0",
       .content =
           v3_nav_first +
           HeaderLine("GPUT BAD_A0           2.500000000D-16 147456 2086",
                      "TIME SYSTEM CORR"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "bad_tsc_a1",
       .content =
           v3_nav_first +
           HeaderLine("GPUT  1.2500000000D-09BAD_A1          147456 2086",
                      "TIME SYSTEM CORR"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "bad_tsc_t",
       .content =
           v3_nav_first +
           HeaderLine("GPUT  1.2500000000D-09 2.500000000D-16BAD_T  2086",
                      "TIME SYSTEM CORR"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "bad_tsc_w",
       .content =
           v3_nav_first +
           HeaderLine("GPUT  1.2500000000D-09 2.500000000D-16 147456BADW",
                      "TIME SYSTEM CORR"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "bad_tsc_sbas_u",
       .content =
           v3_nav_first +
           HeaderLine(
               "SBUT  1.0000000000D-09 2.000000000D-16  86400 2086 EGNOS XX",
               "TIME SYSTEM CORR"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "bad_nav_leap_seconds",
       .content = v3_nav_first + HeaderLine("BAD", "LEAP SECONDS"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "rec_line_too_short",
       .content = base_hdr + "G01 2020 01 01\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "record line length (14) too short"},
      {.name = "rec_bad_sys",
       .content =
           base_hdr + "J01 2020 01 01 00 00 00" + std::string(57, ' ') + "\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "invalid satellite system for navigation record"},
      {.name = "rec_bad_sat_num",
       .content =
           base_hdr + "GXX 2020 01 01 00 00 00" + std::string(57, ' ') + "\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "invalid satellite number"},
      {.name = "rec_bad_value",
       .content =
           base_hdr +
           "G01 2020 01 01 00 00 00    BAD_DOUBLE_VALUE 2.000000000000D-12 "
           "0.000000000000D+00\n",
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 5"},
      {.name = "rec_gps_short_cont",
       .content = base_hdr + valid_nav_line0 + "    SHORT\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "record line length (9) too short"},
      {.name = "rec_glo_short_cont",
       .content = base_hdr +
                  "R02 2020 01 01 00 00 00 1.000000000000D-04 "
                  "2.000000000000D-12 0.000000000000D+00\n    SHORT\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "record line length (9) too short"},
      {.name = "rec_gal_short_cont",
       .content = base_hdr +
                  "E03 2020 01 01 00 00 00 1.000000000000D-04 "
                  "2.000000000000D-12 0.000000000000D+00\n    SHORT\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "record line length (9) too short"},
      {.name = "rec_sbas_short_cont",
       .content = base_hdr +
                  "S20 2020 01 01 00 00 00 1.000000000000D-04 "
                  "2.000000000000D-12 0.000000000000D+00\n    SHORT\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "record line length (9) too short"},
  };

  for (const ErrorCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    TempFile file(absl::StrCat("bad_nav_", tc.name, ".rnx"));
    ASSERT_TRUE(file.WriteText(tc.content));
    ScopedRinexProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, CallAllowingUpstreamRinexLeak([&] {
      return coda_open(file.c_str(), product.OutParam());
    }));
    EXPECT_EQ(tc.expected_errno, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr(tc.expected_msg));
    EXPECT_EQ(nullptr, product.get());
  }
}

TEST(CodaRinexTest, RejectsMalformedClockHeadersAndRecords) {
  ScopedRinexTestEnv env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  struct ErrorCase {
    std::string_view name;
    std::string content;
    int expected_errno;
    std::string_view expected_msg;
  };

  const std::string v3_clk_first = HeaderLine(
      "     3.00           C                   M", "RINEX VERSION / TYPE");
  const std::string base_hdr = MakeMinimalClockHeader();

  const ErrorCase kCases[] = {
      {.name = "short_hdr_line",
       .content = v3_clk_first + "SHORT\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "header line length (5) too short"},
      {.name = "unknown_hdr_item",
       .content = v3_clk_first + HeaderLine("DATA", "UNKNOWN CLK ITEM"),
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "invalid header item"},
      {.name = "bad_clk_obs_def",
       .content =
           v3_clk_first + HeaderLine("J    1 C1C", "SYS / # / OBS TYPES"),
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "invalid satellite system for observation type"},
      {.name = "bad_clk_leap_seconds",
       .content = v3_clk_first + HeaderLine("BAD", "LEAP SECONDS"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 2"},
      {.name = "missing_mandatory_field",
       .content = v3_clk_first + HeaderLine("", "END OF HEADER"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory field"},
      {.name = "rec_line_too_short",
       .content = base_hdr + "AR ALGO 2020 01 01\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "record line length (18) too short"},
      {.name = "rec_num_values_zero",
       .content = base_hdr + MakeClockRecord("AR", "ALGO",
                                             "2020 01 01 00 00  0.000000", 0,
                                             " 1.000000000000E-04"),
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "invalid 'number of data values' entry"},
      {.name = "rec_num_values_seven",
       .content = base_hdr + MakeClockRecord("AR", "ALGO",
                                             "2020 01 01 00 00  0.000000", 7,
                                             " 1.000000000000E-04"),
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "invalid 'number of data values' entry"},
      {.name = "rec_num_values_non_numeric",
       .content = base_hdr +
                  "AR ALGO 2020 01 01 00 00  0.000000 XX  1.000000000000E-04\n",
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "invalid 'number of data values' entry"},
      {.name = "rec_bad_bias",
       .content = base_hdr + MakeClockRecord("AR", "ALGO",
                                             "2020 01 01 00 00  0.000000", 1,
                                             " BAD_BIAS_VALUE    "),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 5"},
      {.name = "rec_short_for_sigma",
       .content = base_hdr + MakeClockRecord("AR", "ALGO",
                                             "2020 01 01 00 00  0.000000", 2,
                                             " 1.000000000000E-04"),
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "record line length (59) too short"},
      {.name = "rec_bad_bias_sigma",
       .content = base_hdr +
                  MakeClockRecord("AR", "ALGO", "2020 01 01 00 00  0.000000", 2,
                                  " 1.000000000000E-04", " BAD_BIAS_SIGMA    "),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 5"},
      {.name = "rec_short_cont_line",
       .content = base_hdr + MakeClockRecord("AR", "ALGO",
                                             "2020 01 01 00 00  0.000000", 3,
                                             " 1.000000000000E-04",
                                             " 1.000000000000E-10", "SHORT"),
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "record line length (5) too short"},
      {.name = "rec_short_cont_for_6_values",
       .content = base_hdr +
                  MakeClockRecord("AR", "ALGO", "2020 01 01 00 00  0.000000", 6,
                                  " 1.000000000000E-04", " 1.000000000000E-10",
                                  " 1.000000000000E-11  2.000000000000E-13"),
       .expected_errno = CODA_ERROR_FILE_READ,
       .expected_msg = "record line length (39) too short"},
      {.name = "rec_bad_rate",
       .content = base_hdr +
                  MakeClockRecord("AR", "ALGO", "2020 01 01 00 00  0.000000", 3,
                                  " 1.000000000000E-04", " 1.000000000000E-10",
                                  " BAD_RATE_VALUE    "),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 6"},
      {.name = "rec_bad_rate_sigma",
       .content = base_hdr +
                  MakeClockRecord("AR", "ALGO", "2020 01 01 00 00  0.000000", 4,
                                  " 1.000000000000E-04", " 1.000000000000E-10",
                                  " 1.000000000000E-11  BAD_RATE_SIGMA    "),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 6"},
      {.name = "rec_bad_acceleration",
       .content = base_hdr +
                  MakeClockRecord("AR", "ALGO", "2020 01 01 00 00  0.000000", 5,
                                  " 1.000000000000E-04", " 1.000000000000E-10",
                                  " 1.000000000000E-11  2.000000000000E-13 "
                                  "   BAD_ACCELERATION"),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 6"},
      {.name = "rec_bad_acceleration_sigma",
       .content = base_hdr +
                  MakeClockRecord("AR", "ALGO", "2020 01 01 00 00  0.000000", 6,
                                  " 1.000000000000E-04", " 1.000000000000E-10",
                                  " 1.000000000000E-11  2.000000000000E-13 "
                                  " 3.000000000000E-15  BAD_ACC_SIGMA     "),
       .expected_errno = CODA_ERROR_INVALID_FORMAT,
       .expected_msg = "line: 6"},
  };

  for (const ErrorCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    TempFile file(absl::StrCat("bad_clk_", tc.name, ".clk"));
    ASSERT_TRUE(file.WriteText(tc.content));
    ScopedRinexProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, CallAllowingUpstreamRinexLeak([&] {
      return coda_open(file.c_str(), product.OutParam());
    }));
    EXPECT_EQ(tc.expected_errno, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr(tc.expected_msg));
    EXPECT_EQ(nullptr, product.get());
  }
}

TEST(CodaRinexTest,
     SingleSystemObservationWithEpochAndHeadersWithoutEndOfHeader) {
  ScopedRinexTestEnv env;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Single-constellation ('R') Observation v3 file exercises NULL
  //    gps/galileo/sbas sat_obs_array_definition checks in
  //    read_observation_records(), a 0-satellite epoch, and 1-char trailing
  //    blank padding (omitted signal_strength).
  {
    std::string glo_obs;
    glo_obs += HeaderLine("     3.00           O                   R",
                          "RINEX VERSION / TYPE");
    glo_obs += HeaderLine(
        "CODATEST            STCORP              20200101 000000 UTC",
        "PGM / RUN BY / DATE");
    glo_obs += HeaderLine("GLO_MARKER", "MARKER NAME");
    glo_obs += HeaderLine("OBS                 AG", "OBSERVER / AGENCY");
    glo_obs += HeaderLine("R1                  T1                  V1",
                          "REC # / TYPE / VERS");
    glo_obs += HeaderLine("A1                  T1", "ANT # / TYPE");
    glo_obs += HeaderLine("        0.0000        0.0000        0.0000",
                          "ANTENNA: DELTA H/E/N");
    glo_obs += HeaderLine("R    1 C1C", "SYS / # / OBS TYPES");
    glo_obs += HeaderLine("  2020    01    01    00    00   00.0000000",
                          "TIME OF FIRST OBS");
    glo_obs += HeaderLine("", "END OF HEADER");
    glo_obs += "> 2020 01 01 00 00  0.0000000  0  0\n";
    glo_obs += "> 2020 01 01 00 01  0.0000000  0  1\n";
    glo_obs += "R01  12345678.5001\n";

    TempFile file("rinex_glo_only.rnx");
    ASSERT_TRUE(file.WriteText(glo_obs));
    ScopedRinexProduct product;
    ASSERT_EQ(CODA_SUCCESS, CallAllowingUpstreamRinexLeak([&] {
                return coda_open(file.c_str(), product.OutParam());
              }));

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

    coda_cursor rec0 = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&rec0, "/record[0]/glonass"));
    long num_glo = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&rec0, &num_glo));
    EXPECT_EQ(0L, num_glo);

    coda_cursor rec1 = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&rec1, "/record[1]"));
    coda_type* rec1_type = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(&rec1, &rec1_type));
    long num_fields = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_get_num_record_fields(rec1_type, &num_fields));
    // Only epoch, flag, receiver_clock_offset, and glonass are defined.
    EXPECT_EQ(4L, num_fields);

    EXPECT_EQ(1, ReadField(cursor, "/record[1]/glonass[0]/number",
                           coda_cursor_read_uint8));
    EXPECT_DOUBLE_EQ(12345678.5,
                     ReadField(cursor, "/record[1]/glonass[0]/C1C/observation",
                               coda_cursor_read_double));
    EXPECT_EQ(1, ReadField(cursor, "/record[1]/glonass[0]/C1C/lli",
                           coda_cursor_read_uint8));
    EXPECT_EQ(0, ReadField(cursor, "/record[1]/glonass[0]/C1C/signal_strength",
                           coda_cursor_read_uint8));
  }

  // 2. Observation, Navigation, and Clock headers ending at EOF without
  //    END OF HEADER exercise normal while (linelength > 0) loop termination.
  struct NoEndOfHeaderCase {
    std::string_view name;
    std::string content;
    const char* field_path;
    std::string_view expected_value;
  };
  auto strip_end_of_header = [](std::string hdr) {
    const std::string eoh = HeaderLine("", "END OF HEADER");
    if (hdr.size() >= eoh.size() && std::string_view(hdr).ends_with(eoh)) {
      hdr.resize(hdr.size() - eoh.size());
    }
    return hdr;
  };
  const NoEndOfHeaderCase kNoEohCases[] = {
      {.name = "obs_no_eoh.rnx",
       .content = strip_end_of_header(MakeMinimalObsV3Header('G')),
       .field_path = "/header/marker_name",
       .expected_value = "TEST_MARKER"},
      {.name = "nav_no_eoh.rnx",
       .content = strip_end_of_header(MakeMinimalNavV3Header('G')),
       .field_path = "/header/program",
       .expected_value = "CODATEST"},
      {.name = "clk_no_eoh.clk",
       .content = strip_end_of_header(MakeMinimalClockHeader()),
       .field_path = "/header/program",
       .expected_value = "CODATEST"},
  };
  for (const NoEndOfHeaderCase& tc : kNoEohCases) {
    SCOPED_TRACE(tc.name);
    TempFile file(tc.name);
    ASSERT_TRUE(file.WriteText(tc.content));
    ScopedRinexProduct product;
    ASSERT_EQ(CODA_SUCCESS, CallAllowingUpstreamRinexLeak([&] {
                return coda_open(file.c_str(), product.OutParam());
              }));
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    EXPECT_EQ(tc.expected_value, ReadFieldString(cursor, tc.field_path));
  }
}

// TODO(schwehr): Remaining uncovered lines in libcoda/coda-rinex.c are
// malloc/strdup OOM checks, ferror() I/O failure branches in get_line(), and
// compiled-out WORDS_BIGENDIAN paths.

}  // namespace
}  // namespace stcorp_coda
