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
// Comprehensive unit tests for GSF core file I/O, indexing, record copying,
// parameter serialization, scaling, and error reporting (src/gsf.c).
//
// Build caveats in google3: //third_party/gsf is compiled with
// _LARGEFILE64_SOURCE defined on Linux, Windows/OS2 branches disabled, and
// DISPLAY_SPINNER unset in gsf_indx.c.
//
// See also:
//   https://github.com/schwehr/generic-sensor-format-3/blob/main/src/gsf.c
//   https://github.com/schwehr/generic-sensor-format-3/blob/main/src/gsf.h

#include "third_party/gsf/src/gsf.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>  // NOLINT
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gsf/google/util/gsf_internal.h"
#include "third_party/gsf/google/util/temp_file.h"

namespace gsf {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr int kBadHandleZero = 0;
constexpr int kBadHandleOver = GSF_MAX_OPEN_FILES + 1;

// Explicitly zeroes C structs including union tails and padding bytes for MSan.
template <typename... Ts>
void ZeroStructs(Ts&... objs) {
  static_assert((std::is_trivially_copyable_v<Ts> && ...));
  (std::memset(static_cast<void*>(&objs), 0, sizeof(Ts)), ...);
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t val) {
  out.push_back(static_cast<uint8_t>((val >> 24) & 0xFFu));
  out.push_back(static_cast<uint8_t>((val >> 16) & 0xFFu));
  out.push_back(static_cast<uint8_t>((val >> 8) & 0xFFu));
  out.push_back(static_cast<uint8_t>(val & 0xFFu));
}

template <size_t N>
void CopyToFixedChars(char (&dst)[N], std::string_view src) {
  const size_t n = std::min(src.size(), N - 1);
  std::copy_n(src.data(), n, dst);
  dst[n] = '\0';
}

// Appends a raw framed GSF record with big-endian size/ID words and optional
// 32-bit modulo byte-sum checksum.
void AppendRawRecord(std::vector<uint8_t>& out, bool checksum_flag,
                     uint32_t reserved, uint32_t record_id,
                     std::span<const uint8_t> payload,
                     bool corrupt_checksum = false) {
  uint32_t did = (reserved & 0x1FFu) << 22 | (record_id & 0x3FFFFFu);
  if (checksum_flag) {
    did |= 0x80000000u;
  }
  AppendBe32(out, static_cast<uint32_t>(payload.size()));
  AppendBe32(out, did);
  if (checksum_flag) {
    uint32_t sum = 0;
    for (uint8_t b : payload) {
      sum += b;
    }
    if (corrupt_checksum) {
      sum ^= 0x00FF00FFu;
    }
    AppendBe32(out, sum);
  }
  out.insert(out.end(), payload.begin(), payload.end());
}

std::vector<uint8_t> MakeHeaderPayload(std::string_view version_text) {
  std::vector<uint8_t> payload(GSF_VERSION_SIZE, 0);
  const size_t copy_len =
      version_text.size() < payload.size() - 1 ? version_text.size()
                                               : payload.size() - 1;
  std::memcpy(payload.data(), version_text.data(), copy_len);
  return payload;
}

void WriteBytesToFile(const std::filesystem::path& path,
                      std::span<const uint8_t> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << path;
  if (!bytes.empty()) {
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  ASSERT_TRUE(out.good()) << path;
}

// Closes handle and clears DecodeBRBIntensity's per-slot arraySize sentinel so
// subsequent slot reuse frees static samplesArraySize cleanly (gsf_dec.c:7831).
void CloseAndResetBrb(int handle) {
  if (handle >= 1 && handle <= GSF_MAX_OPEN_FILES) {
    gsfClose(handle);
    arraySize[handle - 1]
             [GSF_SWATH_BATHY_SUBRECORD_INTENSITY_SERIES_ARRAY - 1] = 0;
  }
}

// Snapshots process_parameters.param pointers before gsfPutMBParams and frees
// any pointer replaced when a parameter string grows (gsf.c:3832 leak quirk).
// Precondition: `rec` must be zeroed or populated exclusively by `gsfPutMBParams`
// / `gsfRead` on `handle` (never caller-owned/stack storage in `param[i]`).
int PutMBParamsSafely(const gsfMBParams* params, gsfRecords* rec, int handle,
                      int num_arrays) {
  std::array<char*, GSF_MAX_PROCESSING_PARAMETERS> prev_ptrs{};
  for (int i = 0; i < GSF_MAX_PROCESSING_PARAMETERS; ++i) {
    prev_ptrs[i] = rec->process_parameters.param[i];
  }
  const int status = gsfPutMBParams(params, rec, handle, num_arrays);
  for (int i = 0; i < GSF_MAX_PROCESSING_PARAMETERS; ++i) {
    if (prev_ptrs[i] != nullptr && rec->process_parameters.param[i] != nullptr &&
        rec->process_parameters.param[i] != prev_ptrs[i]) {
      std::free(prev_ptrs[i]);
    }
  }
  return status;
}

// Owns backing storage for all 30 swath beam arrays plus brb_inten so pings can
// be constructed cleanly for both gsfWrite and gsfCopyRecords tests.
struct OwnedPingArrays {
  explicit OwnedPingArrays(int beams, int samples_per_beam = 2,
                           bool include_all = true)
      : num_beams(beams),
        include_brb_and_unrealloc(include_all),
        depth(beams),
        nominal_depth(beams),
        across_track(beams),
        along_track(beams),
        travel_time(beams),
        beam_angle(beams),
        mc_amplitude(beams),
        mr_amplitude(beams),
        echo_width(beams),
        quality_factor(beams),
        receive_heave(beams),
        depth_error(beams),
        across_track_error(beams),
        along_track_error(beams),
        quality_flags(beams),
        beam_flags(beams),
        signal_to_noise(beams),
        beam_angle_forward(beams),
        vertical_error(beams),
        horizontal_error(beams),
        sector_number(beams),
        detection_info(beams),
        incident_beam_adj(beams),
        system_cleaning(beams),
        doppler_corr(beams),
        sonar_vert_uncert(beams),
        sonar_horz_uncert(beams),
        detection_window(beams),
        mean_abs_coeff(beams),
        tvg_db(beams),
        time_series(beams),
        samples(beams, std::vector<unsigned int>(samples_per_beam)) {
    if (!time_series.empty()) {
      std::memset(time_series.data(), 0,
                  time_series.size() * sizeof(gsfTimeSeriesIntensity));
    }
    ZeroStructs(brb_inten);
    for (int i = 0; i < beams; ++i) {
      const double b = static_cast<double>(i);
      depth[i] = 100.0 + b * 1.25;
      nominal_depth[i] = 99.0 + b * 1.0;
      across_track[i] = -20.0 + b * 10.0;
      along_track[i] = -5.0 + b * 2.5;
      travel_time[i] = 0.13 + b * 0.01;
      beam_angle[i] = -30.0 + b * 15.0;
      // Keep scaled 1-byte signed arrays positive in [10..100] for ARM
      // -funsigned-char parity.
      mc_amplitude[i] = 10.0 + b * 5.0;
      mr_amplitude[i] = 15.0 + b * 5.0;
      echo_width[i] = 0.01 + b * 0.005;
      quality_factor[i] = 1.0 + b;
      receive_heave[i] = 0.1 + b * 0.1;
      depth_error[i] = 0.1 + b * 0.05;
      across_track_error[i] = 0.2 + b * 0.05;
      along_track_error[i] = 0.3 + b * 0.05;
      quality_flags[i] = static_cast<unsigned char>(i % 4);
      beam_flags[i] = static_cast<unsigned char>(
          i == 1 ? GSF_IGNORE_BEAM
                 : (i == 2 ? GSF_BEAM_USER_FLAG_01 : 0u));
      signal_to_noise[i] = 10.0 + b * 4.0;
      beam_angle_forward[i] = 1.0 + b;
      vertical_error[i] = 0.15 + b * 0.05;
      horizontal_error[i] = 0.25 + b * 0.05;
      sector_number[i] = static_cast<unsigned short>(i % 3);
      detection_info[i] = static_cast<unsigned short>(i + 1);
      incident_beam_adj[i] = 0.5 + b * 0.5;
      system_cleaning[i] = static_cast<unsigned short>(i % 2);
      doppler_corr[i] = 0.2 + b * 0.2;
      sonar_vert_uncert[i] = 0.11 + b * 0.05;
      sonar_horz_uncert[i] = 0.21 + b * 0.05;
      detection_window[i] = 0.05 + b * 0.01;
      mean_abs_coeff[i] = 1.1 + b * 0.1;
      tvg_db[i] = 20.0 + b * 2.5;

      time_series[i].sample_count =
          static_cast<unsigned short>(samples_per_beam);
      time_series[i].detect_sample = 1;
      time_series[i].start_range_samples = static_cast<unsigned short>(i * 10);
      for (int s = 0; s < samples_per_beam; ++s) {
        samples[i][s] = static_cast<unsigned int>(100 + i * 20 + s);
      }
      time_series[i].samples = samples[i].data();
    }
    brb_inten.bits_per_sample = 16;
    brb_inten.applied_corrections =
        GSF_INTENSITY_LINEAR | GSF_INTENSITY_CALIBRATED;
    brb_inten.time_series = time_series.data();
  }

  OwnedPingArrays(const OwnedPingArrays&) = delete;
  OwnedPingArrays& operator=(const OwnedPingArrays&) = delete;
  OwnedPingArrays(OwnedPingArrays&&) = delete;
  OwnedPingArrays& operator=(OwnedPingArrays&&) = delete;

  void Bind(gsfSwathBathyPing& ping) {
    ZeroStructs(ping);
    ping.ping_time.tv_sec = 1700000000;
    ping.ping_time.tv_nsec = 123456000;
    ping.latitude = 37.5;
    ping.longitude = -122.25;
    ping.height = 12.5;
    ping.sep = -30.5;
    ping.number_beams = static_cast<short>(num_beams);
    ping.center_beam = static_cast<short>(num_beams / 2);
    ping.ping_flags = GSF_PING_USER_FLAG_01 | GSF_PING_USER_FLAG_14;
    ping.reserved = 7;
    ping.tide_corrector = 1.25;
    ping.gps_tide_corrector = -0.5;
    ping.depth_corrector = 0.75;
    ping.heading = 90.0;
    ping.pitch = 1.5;
    ping.roll = -2.0;
    ping.heave = 0.25;
    ping.course = 89.5;
    ping.speed = 10.5;
    ping.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC;
    ping.sensor_data.gsfReson8100Specific.latency = 5;
    ping.sensor_data.gsfReson8100Specific.ping_number = 42;
    ping.sensor_data.gsfReson8100Specific.surface_velocity = 1500.0;
    ping.sensor_data.gsfReson8100Specific.fore_aft_bw = 1.5;
    ping.sensor_data.gsfReson8100Specific.athwart_bw = 1.5;

    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY, GSF_FIELD_SIZE_TWO, 0.01, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_NOMINAL_DEPTH_ARRAY, GSF_FIELD_SIZE_TWO, 0.01, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY, GSF_FIELD_SIZE_TWO, 0.01, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_ALONG_TRACK_ARRAY, GSF_FIELD_SIZE_TWO, 0.01, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_TRAVEL_TIME_ARRAY, GSF_FIELD_SIZE_TWO, 0.001, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_BEAM_ANGLE_ARRAY, GSF_FIELD_SIZE_DEFAULT, 0.01, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_MEAN_CAL_AMPLITUDE_ARRAY, GSF_FIELD_SIZE_ONE, 0.5, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_MEAN_REL_AMPLITUDE_ARRAY, GSF_FIELD_SIZE_ONE, 0.5, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_ECHO_WIDTH_ARRAY, GSF_FIELD_SIZE_ONE, 0.001, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_QUALITY_FACTOR_ARRAY, GSF_FIELD_SIZE_DEFAULT, 1.0, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_RECEIVE_HEAVE_ARRAY, GSF_FIELD_SIZE_DEFAULT, 0.1, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ERROR_ARRAY, GSF_FIELD_SIZE_DEFAULT, 0.01, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ERROR_ARRAY, GSF_FIELD_SIZE_DEFAULT, 0.01, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_ALONG_TRACK_ERROR_ARRAY, GSF_FIELD_SIZE_DEFAULT, 0.01, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_SIGNAL_TO_NOISE_ARRAY, GSF_FIELD_SIZE_DEFAULT, 1.0, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_BEAM_ANGLE_FORWARD_ARRAY, GSF_FIELD_SIZE_DEFAULT, 0.01, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_VERTICAL_ERROR_ARRAY, GSF_FIELD_SIZE_DEFAULT, 0.01, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_HORIZONTAL_ERROR_ARRAY, GSF_FIELD_SIZE_DEFAULT, 0.01, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_SECTOR_NUMBER_ARRAY, GSF_FIELD_SIZE_DEFAULT, 1.0, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_DETECTION_INFO_ARRAY, GSF_FIELD_SIZE_DEFAULT, 1.0, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_INCIDENT_BEAM_ADJ_ARRAY, GSF_FIELD_SIZE_DEFAULT, 0.1, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_SYSTEM_CLEANING_ARRAY, GSF_FIELD_SIZE_DEFAULT, 1.0, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_DOPPLER_CORRECTION_ARRAY, GSF_FIELD_SIZE_DEFAULT, 0.1, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_SONAR_VERT_UNCERT_ARRAY, GSF_FIELD_SIZE_DEFAULT, 0.01, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_SONAR_HORZ_UNCERT_ARRAY, GSF_FIELD_SIZE_DEFAULT, 0.01, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_DETECTION_WINDOW_ARRAY, GSF_FIELD_SIZE_TWO, 0.001, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_MEAN_ABS_COEF_ARRAY, GSF_FIELD_SIZE_TWO, 0.01, 0));
    ASSERT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_TVG_ARRAY, GSF_FIELD_SIZE_TWO, 0.1, 0));

    ping.depth = depth.data();
    ping.nominal_depth = nominal_depth.data();
    ping.across_track = across_track.data();
    ping.along_track = along_track.data();
    ping.travel_time = travel_time.data();
    ping.beam_angle = beam_angle.data();
    ping.mc_amplitude = mc_amplitude.data();
    ping.mr_amplitude = mr_amplitude.data();
    ping.echo_width = echo_width.data();
    ping.quality_factor = quality_factor.data();
    ping.depth_error = depth_error.data();
    ping.across_track_error = across_track_error.data();
    ping.along_track_error = along_track_error.data();
    ping.quality_flags = quality_flags.data();
    ping.beam_flags = beam_flags.data();
    ping.signal_to_noise = signal_to_noise.data();
    ping.beam_angle_forward = beam_angle_forward.data();
    ping.horizontal_error = horizontal_error.data();
    ping.sector_number = sector_number.data();
    ping.detection_info = detection_info.data();
    ping.incident_beam_adj = incident_beam_adj.data();
    ping.system_cleaning = system_cleaning.data();
    ping.doppler_corr = doppler_corr.data();
    ping.sonar_vert_uncert = sonar_vert_uncert.data();
    ping.sonar_horz_uncert = sonar_horz_uncert.data();
    ping.detection_window = detection_window.data();
    ping.mean_abs_coeff = mean_abs_coeff.data();
    ping.TVG_dB = tvg_db.data();
    if (include_brb_and_unrealloc) {
      ping.receive_heave = receive_heave.data();
      ping.vertical_error = vertical_error.data();
      ping.brb_inten = &brb_inten;
    }
  }

  int num_beams;
  bool include_brb_and_unrealloc;
  std::vector<double> depth;
  std::vector<double> nominal_depth;
  std::vector<double> across_track;
  std::vector<double> along_track;
  std::vector<double> travel_time;
  std::vector<double> beam_angle;
  std::vector<double> mc_amplitude;
  std::vector<double> mr_amplitude;
  std::vector<double> echo_width;
  std::vector<double> quality_factor;
  std::vector<double> receive_heave;
  std::vector<double> depth_error;
  std::vector<double> across_track_error;
  std::vector<double> along_track_error;
  std::vector<unsigned char> quality_flags;
  std::vector<unsigned char> beam_flags;
  std::vector<double> signal_to_noise;
  std::vector<double> beam_angle_forward;
  std::vector<double> vertical_error;
  std::vector<double> horizontal_error;
  std::vector<unsigned short> sector_number;
  std::vector<unsigned short> detection_info;
  std::vector<double> incident_beam_adj;
  std::vector<unsigned short> system_cleaning;
  std::vector<double> doppler_corr;
  std::vector<double> sonar_vert_uncert;
  std::vector<double> sonar_horz_uncert;
  std::vector<double> detection_window;
  std::vector<double> mean_abs_coeff;
  std::vector<double> tvg_db;
  std::vector<gsfTimeSeriesIntensity> time_series;
  std::vector<std::vector<unsigned int>> samples;
  gsfBRBIntensity brb_inten{};
};

// Populates all 53 offset fields encoded/decoded by gsfPutMBParams/gsfGetMBParams
// with exact base-2 decimal fractions so round-trip comparisons are exact.
void FillMBOffsets(gsfMBOffsets& offsets, double base) {
  offsets.position_x_offset = base + 0.25;
  offsets.position_y_offset = base + 0.50;
  offsets.position_z_offset = base + 0.75;
  offsets.antenna_x_offset = base + 1.00;
  offsets.antenna_y_offset = base + 1.25;
  offsets.antenna_z_offset = base + 1.50;
  offsets.mru_pitch_bias = base + 1.75;
  offsets.mru_roll_bias = base + 2.00;
  offsets.mru_heading_bias = base + 2.25;
  offsets.mru_x_offset = base + 2.50;
  offsets.mru_y_offset = base + 2.75;
  offsets.mru_z_offset = base + 3.00;
  offsets.center_of_rotation_x_offset = base + 3.25;
  offsets.center_of_rotation_y_offset = base + 3.50;
  offsets.center_of_rotation_z_offset = base + 3.75;
  offsets.position_latency = base + 0.125;
  offsets.attitude_latency = base + 0.250;
  offsets.depth_sensor_latency = base + 0.375;
  offsets.depth_sensor_x_offset = base + 4.00;
  offsets.depth_sensor_y_offset = base + 4.25;
  offsets.depth_sensor_z_offset = base + 4.50;

  for (int i = 0; i < GSF_MAX_OFFSETS; ++i) {
    const double s = base + static_cast<double>(i) * 10.0;
    offsets.draft[i] = s + 5.00;
    offsets.pitch_bias[i] = s + 5.25;
    offsets.roll_bias[i] = s + 5.50;
    offsets.gyro_bias[i] = s + 5.75;
    offsets.transducer_x_offset[i] = s + 6.00;
    offsets.transducer_y_offset[i] = s + 6.25;
    offsets.transducer_z_offset[i] = s + 6.50;
    offsets.transducer_pitch_offset[i] = s + 6.75;
    offsets.transducer_roll_offset[i] = s + 7.00;
    offsets.transducer_heading_offset[i] = s + 7.25;
    offsets.rx_transducer_x_offset[i] = s + 7.50;
    offsets.rx_transducer_y_offset[i] = s + 7.75;
    offsets.rx_transducer_z_offset[i] = s + 8.00;
    offsets.rx_transducer_pitch_offset[i] = s + 8.25;
    offsets.rx_transducer_roll_offset[i] = s + 8.50;
    offsets.rx_transducer_heading_offset[i] = s + 8.75;
  }
}

void ExpectOffsetsEqual(const gsfMBOffsets& expected,
                        const gsfMBOffsets& actual, int num_arrays) {
  EXPECT_DOUBLE_EQ(expected.position_x_offset, actual.position_x_offset);
  EXPECT_DOUBLE_EQ(expected.position_y_offset, actual.position_y_offset);
  EXPECT_DOUBLE_EQ(expected.position_z_offset, actual.position_z_offset);
  EXPECT_DOUBLE_EQ(expected.antenna_x_offset, actual.antenna_x_offset);
  EXPECT_DOUBLE_EQ(expected.antenna_y_offset, actual.antenna_y_offset);
  EXPECT_DOUBLE_EQ(expected.antenna_z_offset, actual.antenna_z_offset);
  EXPECT_DOUBLE_EQ(expected.mru_pitch_bias, actual.mru_pitch_bias);
  EXPECT_DOUBLE_EQ(expected.mru_roll_bias, actual.mru_roll_bias);
  EXPECT_DOUBLE_EQ(expected.mru_heading_bias, actual.mru_heading_bias);
  EXPECT_DOUBLE_EQ(expected.mru_x_offset, actual.mru_x_offset);
  EXPECT_DOUBLE_EQ(expected.mru_y_offset, actual.mru_y_offset);
  EXPECT_DOUBLE_EQ(expected.mru_z_offset, actual.mru_z_offset);
  EXPECT_DOUBLE_EQ(expected.center_of_rotation_x_offset,
                   actual.center_of_rotation_x_offset);
  EXPECT_DOUBLE_EQ(expected.center_of_rotation_y_offset,
                   actual.center_of_rotation_y_offset);
  EXPECT_DOUBLE_EQ(expected.center_of_rotation_z_offset,
                   actual.center_of_rotation_z_offset);
  EXPECT_DOUBLE_EQ(expected.position_latency, actual.position_latency);
  EXPECT_DOUBLE_EQ(expected.attitude_latency, actual.attitude_latency);
  EXPECT_DOUBLE_EQ(expected.depth_sensor_latency, actual.depth_sensor_latency);
  EXPECT_DOUBLE_EQ(expected.depth_sensor_x_offset,
                   actual.depth_sensor_x_offset);
  EXPECT_DOUBLE_EQ(expected.depth_sensor_y_offset,
                   actual.depth_sensor_y_offset);
  EXPECT_DOUBLE_EQ(expected.depth_sensor_z_offset,
                   actual.depth_sensor_z_offset);

  for (int i = 0; i < num_arrays; ++i) {
    EXPECT_DOUBLE_EQ(expected.draft[i], actual.draft[i]);
    EXPECT_DOUBLE_EQ(expected.pitch_bias[i], actual.pitch_bias[i]);
    EXPECT_DOUBLE_EQ(expected.roll_bias[i], actual.roll_bias[i]);
    EXPECT_DOUBLE_EQ(expected.gyro_bias[i], actual.gyro_bias[i]);
    EXPECT_DOUBLE_EQ(expected.transducer_x_offset[i],
                     actual.transducer_x_offset[i]);
    EXPECT_DOUBLE_EQ(expected.transducer_y_offset[i],
                     actual.transducer_y_offset[i]);
    EXPECT_DOUBLE_EQ(expected.transducer_z_offset[i],
                     actual.transducer_z_offset[i]);
    EXPECT_DOUBLE_EQ(expected.transducer_pitch_offset[i],
                     actual.transducer_pitch_offset[i]);
    EXPECT_DOUBLE_EQ(expected.transducer_roll_offset[i],
                     actual.transducer_roll_offset[i]);
    EXPECT_DOUBLE_EQ(expected.transducer_heading_offset[i],
                     actual.transducer_heading_offset[i]);
    EXPECT_DOUBLE_EQ(expected.rx_transducer_x_offset[i],
                     actual.rx_transducer_x_offset[i]);
    EXPECT_DOUBLE_EQ(expected.rx_transducer_y_offset[i],
                     actual.rx_transducer_y_offset[i]);
    EXPECT_DOUBLE_EQ(expected.rx_transducer_z_offset[i],
                     actual.rx_transducer_z_offset[i]);
    EXPECT_DOUBLE_EQ(expected.rx_transducer_pitch_offset[i],
                     actual.rx_transducer_pitch_offset[i]);
    EXPECT_DOUBLE_EQ(expected.rx_transducer_roll_offset[i],
                     actual.rx_transducer_roll_offset[i]);
    EXPECT_DOUBLE_EQ(expected.rx_transducer_heading_offset[i],
                     actual.rx_transducer_heading_offset[i]);
  }
}

class GsfTest : public ::testing::Test {
 protected:
  GsfTest()
      : temp_dir_(MakeTestDirName()),
        scoped_tmpdir_("GSFTMPDIR", temp_dir_.string()) {}

  void SetUp() override { gsfError = GSF_NORMAL; }
  void TearDown() override { gsfError = GSF_NORMAL; }

  std::filesystem::path TestPath(std::string_view filename) const {
    return temp_dir_.file(filename);
  }

 private:
  static std::string MakeTestDirName() {
    const testing::TestInfo* const info =
        testing::UnitTest::GetInstance()->current_test_info();
    std::string dir = "gsf_test_";
    if (info != nullptr) {
      dir += info->name();
    } else {
      dir += "default";
    }
    return dir;
  }

  TempDir temp_dir_;
  ScopedEnvVar scoped_tmpdir_;
};

// --- Group A: gsfStat, gsfOpen, gsfOpenBuffered, gsfClose ---

TEST_F(GsfTest, StatHandlesNullMissingAndValidFiles) {
  const std::filesystem::path valid_path = TestPath("stat_valid.gsf");
  const std::filesystem::path missing_path = TestPath("does_not_exist.gsf");

  EXPECT_EQ(-1, gsfStat(valid_path.c_str(), nullptr));
  EXPECT_EQ(GSF_FOPEN_ERROR, gsfIntError());

  long long sz = -1;
  EXPECT_EQ(-1, gsfStat(missing_path.c_str(), &sz));
  EXPECT_EQ(GSF_UNRECOGNIZED_FILE, gsfIntError());

  int handle = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(valid_path.c_str(), GSF_CREATE, &handle));
  ASSERT_EQ(GSF_NORMAL, gsfClose(handle));

  // Header is 8 bytes framing + 12 bytes GSF_VERSION_SIZE = 20 bytes.
  EXPECT_EQ(GSF_NORMAL, gsfStat(valid_path.c_str(), &sz));
  EXPECT_EQ(20LL, sz);
}

TEST_F(GsfTest, OpenRejectsInvalidModesMissingFilesAndBadCloseHandles) {
  const std::filesystem::path missing = TestPath("missing.gsf");
  int handle = -1;

  EXPECT_EQ(-1, gsfOpen(missing.c_str(), 99, &handle));
  EXPECT_EQ(GSF_BAD_ACCESS_MODE, gsfIntError());
  EXPECT_EQ(0, handle);

  for (int mode :
       {GSF_READONLY, GSF_UPDATE, GSF_READONLY_INDEX, GSF_UPDATE_INDEX}) {
    handle = -1;
    EXPECT_EQ(-1, gsfOpen(missing.c_str(), mode, &handle)) << "mode=" << mode;
    EXPECT_EQ(GSF_FOPEN_ERROR, gsfIntError()) << "mode=" << mode;
  }

  EXPECT_EQ(-1, gsfClose(kBadHandleZero));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());
  EXPECT_EQ(-1, gsfClose(kBadHandleOver));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());
}

TEST_F(GsfTest, OpenEnforcesMaxOpenFilesAndReusesSlots) {
  std::array<int, GSF_MAX_OPEN_FILES> handles{};
  std::array<std::filesystem::path, GSF_MAX_OPEN_FILES> paths;
  absl::Cleanup close_all = [&handles] {
    for (int h : handles) {
      if (h >= 1 && h <= GSF_MAX_OPEN_FILES) {
        gsfClose(h);
      }
    }
  };
  for (int i = 0; i < GSF_MAX_OPEN_FILES; ++i) {
    paths[i] = TestPath("slot_" + std::to_string(i) + ".gsf");
    ASSERT_EQ(GSF_NORMAL, gsfOpen(paths[i].c_str(), GSF_CREATE, &handles[i]));
  }

  int extra_handle = -1;
  const std::filesystem::path extra_path = TestPath("slot_overflow.gsf");
  EXPECT_EQ(-1, gsfOpen(extra_path.c_str(), GSF_CREATE, &extra_handle));
  EXPECT_EQ(GSF_TOO_MANY_OPEN_FILES, gsfIntError());
  EXPECT_EQ(0, extra_handle);

  // Close slot 0, reopen the exact same path (hits filename-match slot reuse),
  // close it again, then open a distinct path (hits gsfFree eviction branch).
  const int closed_handle = handles[0];
  handles[0] = 0;
  ASSERT_EQ(GSF_NORMAL, gsfClose(closed_handle));

  int reopened = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(paths[0].c_str(), GSF_READONLY, &reopened));
  absl::Cleanup close_reopened = [&reopened] {
    if (reopened > 0) {
      gsfClose(reopened);
    }
  };
  EXPECT_EQ(closed_handle, reopened);
  const int h_close = reopened;
  reopened = 0;
  ASSERT_EQ(GSF_NORMAL, gsfClose(h_close));

  int evicted = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(extra_path.c_str(), GSF_CREATE, &evicted));
  absl::Cleanup close_evicted = [&evicted] {
    if (evicted > 0) {
      gsfClose(evicted);
    }
  };
  EXPECT_EQ(closed_handle, evicted);
}

TEST_F(GsfTest, OpenBufferedCustomSizesAndFlushTransitions) {
  // 1. buf_size == 1 exercises minimal valid setvbuf size and triggers
  // bufferedBytes >= buf_size flush on every write.
  const std::filesystem::path p0 = TestPath("buf_one.gsf");
  int h0 = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpenBuffered(p0.c_str(), GSF_CREATE, &h0, 1));
  {
    absl::Cleanup close_h0 = [h0] { gsfClose(h0); };
    std::string text = "one-byte-buffer";
    gsfDataID id{};
    gsfRecords rec{};
    ZeroStructs(id, rec);
    id.recordID = GSF_RECORD_COMMENT;
    rec.comment.comment_length = static_cast<int>(text.size());
    rec.comment.comment = text.data();
    EXPECT_GT(gsfWrite(h0, &id, &rec), 0);
  }

  // 2. Small 64-byte buffer tests write -> read (LAST_OP_WRITE flush in
  // gsfUnpackStream via GSF_PREVIOUS_RECORD) -> write (LAST_OP_READ flush in
  // gsfWrite) transitions.
  const std::filesystem::path p1 = TestPath("buf_64.gsf");
  int h1 = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpenBuffered(p1.c_str(), GSF_CREATE, &h1, 64));
  {
    absl::Cleanup close_h1 = [h1] { gsfClose(h1); };
    std::string long_comment(80, 'x');
    gsfDataID write_id{};
    gsfRecords write_rec{};
    ZeroStructs(write_id, write_rec);
    write_id.recordID = GSF_RECORD_COMMENT;
    write_rec.comment.comment_length = static_cast<int>(long_comment.size());
    write_rec.comment.comment = long_comment.data();
    ASSERT_GT(gsfWrite(h1, &write_id, &write_rec), 0);

    // GSF_PREVIOUS_RECORD preserves read_write_flag == LAST_OP_WRITE so the
    // following gsfRead hits gsf.c:938 fflush transition.
    ASSERT_EQ(GSF_NORMAL, gsfSeek(h1, GSF_PREVIOUS_RECORD));
    gsfDataID read_id{};
    gsfRecords read_rec{};
    ZeroStructs(read_id, read_rec);
    ASSERT_GT(
        gsfRead(h1, GSF_RECORD_COMMENT, &read_id, &read_rec, nullptr, 0), 0);
    EXPECT_EQ(80, read_rec.comment.comment_length);
    EXPECT_STREQ(long_comment.c_str(), read_rec.comment.comment);

    // Writing again after a read triggers LAST_OP_READ fflush in gsfWrite.
    std::string tail_comment = "tail";
    write_rec.comment.comment_length = static_cast<int>(tail_comment.size());
    write_rec.comment.comment = tail_comment.data();
    EXPECT_GT(gsfWrite(h1, &write_id, &write_rec), 0);
  }

  // 3. Reopen read-only with custom buffer and verify both comments exist.
  int h_ro = 0;
  ASSERT_EQ(GSF_NORMAL,
            gsfOpenBuffered(p1.c_str(), GSF_READONLY, &h_ro, 128));
  absl::Cleanup close_ro = [h_ro] { gsfClose(h_ro); };
  gsfDataID id{};
  gsfRecords rec{};
  ZeroStructs(id, rec);
  ASSERT_GT(gsfRead(h_ro, GSF_RECORD_COMMENT, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(80, rec.comment.comment_length);
  ASSERT_GT(gsfRead(h_ro, GSF_RECORD_COMMENT, &id, &rec, nullptr, 0), 0);
  EXPECT_STREQ("tail", rec.comment.comment);
}

TEST_F(GsfTest, OpenAppendAndUpdateModesIncludingLegacyV0102Padding) {
  // 1. Create file with one comment, close, append a second via GSF_APPEND.
  const std::filesystem::path app_path = TestPath("append.gsf");
  int h = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(app_path.c_str(), GSF_CREATE, &h));
  {
    absl::Cleanup close_h = [h] { gsfClose(h); };
    std::string first = "first";
    gsfDataID id{};
    gsfRecords rec{};
    ZeroStructs(id, rec);
    id.recordID = GSF_RECORD_COMMENT;
    rec.comment.comment_length = static_cast<int>(first.size());
    rec.comment.comment = first.data();
    ASSERT_GT(gsfWrite(h, &id, &rec), 0);
  }

  ASSERT_EQ(GSF_NORMAL, gsfOpen(app_path.c_str(), GSF_APPEND, &h));
  {
    absl::Cleanup close_h = [h] { gsfClose(h); };
    std::string second = "second";
    gsfDataID id{};
    gsfRecords rec{};
    ZeroStructs(id, rec);
    id.recordID = GSF_RECORD_COMMENT;
    rec.comment.comment_length = static_cast<int>(second.size());
    rec.comment.comment = second.data();
    ASSERT_GT(gsfWrite(h, &id, &rec), 0);
  }

  ASSERT_EQ(GSF_NORMAL, gsfOpen(app_path.c_str(), GSF_READONLY, &h));
  {
    absl::Cleanup close_h = [h] { gsfClose(h); };
    gsfDataID id{};
    gsfRecords rec{};
    ZeroStructs(id, rec);
    ASSERT_GT(gsfRead(h, GSF_RECORD_COMMENT, &id, &rec, nullptr, 0), 0);
    EXPECT_STREQ("first", rec.comment.comment);
    ASSERT_GT(gsfRead(h, GSF_RECORD_COMMENT, &id, &rec, nullptr, 0), 0);
    EXPECT_STREQ("second", rec.comment.comment);
  }

  // 2. Synthesize GSF-v01.02 header to cover legacy pre-1.03 padding branch
  // (gsf.c:1620): comment length 2 yields dataSize=14 (pad=2) so both old and
  // new alignment produce 16 payload bytes.
  const std::filesystem::path v102_path = TestPath("legacy_v0102.gsf");
  std::vector<uint8_t> raw_v102;
  AppendRawRecord(raw_v102, /*checksum_flag=*/false, /*reserved=*/0,
                  GSF_RECORD_HEADER, MakeHeaderPayload("GSF-v01.02"));
  WriteBytesToFile(v102_path, raw_v102);

  ASSERT_EQ(GSF_NORMAL, gsfOpen(v102_path.c_str(), GSF_UPDATE, &h));
  {
    absl::Cleanup close_h = [h] { gsfClose(h); };
    std::string two_chars = "ab";
    gsfDataID id{};
    gsfRecords rec{};
    ZeroStructs(id, rec);
    id.recordID = GSF_RECORD_COMMENT;
    rec.comment.comment_length = static_cast<int>(two_chars.size());
    rec.comment.comment = two_chars.data();
    ASSERT_GT(gsfWrite(h, &id, &rec), 0);
  }

  ASSERT_EQ(GSF_NORMAL, gsfOpen(v102_path.c_str(), GSF_READONLY, &h));
  absl::Cleanup close_v102 = [h] { gsfClose(h); };
  gsfDataID id{};
  gsfRecords rec{};
  ZeroStructs(id, rec);
  ASSERT_GT(gsfRead(h, GSF_RECORD_COMMENT, &id, &rec, nullptr, 0), 0);
  EXPECT_STREQ("ab", rec.comment.comment);
}

TEST_F(GsfTest, OpenRejectsTruncatedAndNonGsfHeaders) {
  int handle = -1;

  // 1. Truncated 3-byte file fails header read in GSF_READONLY.
  const std::filesystem::path short_path = TestPath("short.gsf");
  ASSERT_NO_FATAL_FAILURE(
      WriteBytesToFile(short_path, std::array<uint8_t, 3>{0x00, 0x01, 0x02}));
  EXPECT_EQ(-1, gsfOpen(short_path.c_str(), GSF_READONLY, &handle));
  EXPECT_EQ(GSF_HEADER_RECORD_DECODE_FAILED, gsfIntError());
  EXPECT_EQ(0, handle);

  // 2. Truncated non-empty file opened with GSF_APPEND ("a+b") rewinds to read
  // the header (gsf.c:433) and fails with GSF_HEADER_RECORD_DECODE_FAILED.
  const std::filesystem::path trunc_append = TestPath("trunc_append.gsf");
  ASSERT_NO_FATAL_FAILURE(
      WriteBytesToFile(trunc_append, std::array<uint8_t, 2>{0x00, 0x01}));
  handle = -1;
  EXPECT_EQ(-1, gsfOpen(trunc_append.c_str(), GSF_APPEND, &handle));
  EXPECT_EQ(GSF_HEADER_RECORD_DECODE_FAILED, gsfIntError());
  EXPECT_EQ(0, handle);

  // 3. Valid framing without "GSF-" prefix fails with GSF_UNRECOGNIZED_FILE.
  const std::filesystem::path not_gsf = TestPath("not_gsf.gsf");
  std::vector<uint8_t> bad_magic;
  AppendRawRecord(bad_magic, false, 0, GSF_RECORD_HEADER,
                  MakeHeaderPayload("NOT_GSF_FMT"));
  ASSERT_NO_FATAL_FAILURE(WriteBytesToFile(not_gsf, bad_magic));
  handle = -1;
  EXPECT_EQ(-1, gsfOpen(not_gsf.c_str(), GSF_READONLY, &handle));
  EXPECT_EQ(GSF_UNRECOGNIZED_FILE, gsfIntError());
  EXPECT_EQ(0, handle);

  // 4. "GSF-" prefix with malformed version numbers fails version sscanf.
  const std::filesystem::path bad_ver = TestPath("bad_ver.gsf");
  std::vector<uint8_t> bad_ver_bytes;
  AppendRawRecord(bad_ver_bytes, false, 0, GSF_RECORD_HEADER,
                  MakeHeaderPayload("GSF-BADVER"));
  ASSERT_NO_FATAL_FAILURE(WriteBytesToFile(bad_ver, bad_ver_bytes));
  handle = -1;
  EXPECT_EQ(-1, gsfOpen(bad_ver.c_str(), GSF_READONLY, &handle));
  EXPECT_EQ(GSF_UNRECOGNIZED_FILE, gsfIntError());
  EXPECT_EQ(0, handle);
}

TEST_F(GsfTest, OpenIndexedFailsWhenFilenameEndsInNsf) {
  // Replacing file[len - 3] with 'n' collides with ".nsf" itself, triggering
  // GSF_INDEX_FILE_OPEN_ERROR in gsfOpen (gsf.c:504, gsf_indx.c:218).
  const std::filesystem::path nsf_path = TestPath("self_collide.nsf");
  int h = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(nsf_path.c_str(), GSF_CREATE, &h));
  ASSERT_EQ(GSF_NORMAL, gsfClose(h));

  int h_idx = -1;
  EXPECT_EQ(-1, gsfOpen(nsf_path.c_str(), GSF_READONLY_INDEX, &h_idx));
  EXPECT_EQ(GSF_INDEX_FILE_OPEN_ERROR, gsfIntError());
  EXPECT_EQ(0, h_idx);
}

// --- Group B: gsfSeek, gsfPercent, gsfWrite, gsfRead, gsfUnpackStream ---

TEST_F(GsfTest, SeekRewindEndPreviousAndBadOptions) {
  EXPECT_EQ(-1, gsfSeek(kBadHandleZero, GSF_REWIND));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());
  EXPECT_EQ(-1, gsfSeek(kBadHandleOver, GSF_REWIND));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());

  const std::filesystem::path path = TestPath("seek.gsf");
  int h = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_CREATE, &h));
  absl::Cleanup close_h = [h] { gsfClose(h); };

  auto write_comment = [h](std::string text) {
    gsfDataID id{};
    gsfRecords rec{};
    ZeroStructs(id, rec);
    id.recordID = GSF_RECORD_COMMENT;
    rec.comment.comment_length = static_cast<int>(text.size());
    rec.comment.comment = text.data();
    ASSERT_GT(gsfWrite(h, &id, &rec), 0);
  };
  write_comment("alpha");
  write_comment("bravo");

  // 1. Rewind immediately after write flushes LAST_OP_WRITE and resets to byte 0.
  ASSERT_EQ(GSF_NORMAL, gsfSeek(h, GSF_REWIND));
  gsfDataID id{};
  gsfRecords rec{};
  ZeroStructs(id, rec);
  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(GSF_RECORD_HEADER, id.recordID);
  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_STREQ("alpha", rec.comment.comment);
  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_STREQ("bravo", rec.comment.comment);

  // 2. GSF_PREVIOUS_RECORD steps back to the start of the record just read.
  ASSERT_EQ(GSF_NORMAL, gsfSeek(h, GSF_PREVIOUS_RECORD));
  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_STREQ("bravo", rec.comment.comment);

  // 3. GSF_END_OF_FILE after write flushes LAST_OP_WRITE and positions at EOF.
  write_comment("charlie");
  ASSERT_EQ(GSF_NORMAL, gsfSeek(h, GSF_END_OF_FILE));
  EXPECT_EQ(-1, gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0));
  EXPECT_EQ(GSF_READ_TO_END_OF_FILE, gsfIntError());

  // 4. Invalid seek option.
  EXPECT_EQ(-1, gsfSeek(h, 999));
  EXPECT_EQ(GSF_BAD_SEEK_OPTION, gsfIntError());
}

TEST_F(GsfTest, PercentProgressAndHandleValidation) {
  EXPECT_EQ(-1, gsfPercent(kBadHandleZero));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());
  EXPECT_EQ(-1, gsfPercent(kBadHandleOver));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());

  // Closed handle is rejected because occupied == 0 (gsf.c:2752).
  const std::filesystem::path path = TestPath("percent.gsf");
  int h = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_CREATE, &h));
  // Newly created file has initial stsize == 0 recorded in file_size -> 0%.
  EXPECT_EQ(0, gsfPercent(h));
  {
    absl::Cleanup close_h = [h] { gsfClose(h); };
    std::string text = "progress-payload";
    gsfDataID id{};
    gsfRecords rec{};
    ZeroStructs(id, rec);
    id.recordID = GSF_RECORD_COMMENT;
    rec.comment.comment_length = static_cast<int>(text.size());
    rec.comment.comment = text.data();
    ASSERT_GT(gsfWrite(h, &id, &rec), 0);
  }
  EXPECT_EQ(-1, gsfPercent(h));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());

  // Reading sequentially advances gsfPercent toward 100.
  int h_ro = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_READONLY, &h_ro));
  absl::Cleanup close_ro = [h_ro] { gsfClose(h_ro); };
  const int after_header = gsfPercent(h_ro);
  EXPECT_GT(after_header, 0);
  EXPECT_LT(after_header, 100);

  gsfDataID id{};
  gsfRecords rec{};
  ZeroStructs(id, rec);
  ASSERT_GT(gsfRead(h_ro, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(100, gsfPercent(h_ro));
}

TEST_F(GsfTest, WriteAndReadAllRecordTypesAndBeamArraysRoundTrip) {
  const std::filesystem::path path = TestPath("all_records.gsf");
  OwnedPingArrays ping_arrays(/*beams=*/4, /*samples_per_beam=*/3,
                              /*include_all=*/true);

  int h = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_CREATE, &h));
  {
    absl::Cleanup close_h = [h] { CloseAndResetBrb(h); };

    // 1. Summary record with checksum and 9-bit reserved value.
    {
      gsfDataID id{};
      gsfRecords rec{};
      ZeroStructs(id, rec);
      id.checksumFlag = 1;
      id.reserved = 42;
      id.recordID = GSF_RECORD_SWATH_BATHY_SUMMARY;
      rec.summary.start_time = {1700000000, 1000};
      rec.summary.end_time = {1700000100, 2000};
      rec.summary.min_latitude = 37.0;
      rec.summary.min_longitude = -123.0;
      rec.summary.max_latitude = 38.0;
      rec.summary.max_longitude = -122.0;
      rec.summary.min_depth = 10.5;
      rec.summary.max_depth = 205.5;
      ASSERT_GT(gsfWrite(h, &id, &rec), 0);
    }

    // 2. Swath bathymetry ping containing all 30 numeric/flag arrays + brb_inten.
    {
      gsfDataID id{};
      gsfRecords rec{};
      ZeroStructs(id, rec);
      id.checksumFlag = 1;
      id.reserved = 7;
      id.recordID = GSF_RECORD_SWATH_BATHYMETRY_PING;
      ASSERT_NO_FATAL_FAILURE(ping_arrays.Bind(rec.mb_ping));
      ASSERT_GT(gsfWrite(h, &id, &rec), 0);
    }

    // 3. Single beam ping.
    {
      gsfDataID id{};
      gsfRecords rec{};
      ZeroStructs(id, rec);
      id.recordID = GSF_RECORD_SINGLE_BEAM_PING;
      rec.sb_ping.ping_time = {1700000001, 500000};
      rec.sb_ping.latitude = 37.25;
      rec.sb_ping.longitude = -122.5;
      rec.sb_ping.tide_corrector = 0.5;
      rec.sb_ping.depth_corrector = 1.25;
      rec.sb_ping.heading = 180.0;
      rec.sb_ping.pitch = 0.5;
      rec.sb_ping.roll = -0.5;
      rec.sb_ping.heave = 0.1;
      rec.sb_ping.depth = 45.25;
      rec.sb_ping.sound_speed_correction = 0.2;
      rec.sb_ping.positioning_system_type = 1;
      rec.sb_ping.sensor_id = GSF_SINGLE_BEAM_SUBRECORD_ECHOTRAC_SPECIFIC;
      rec.sb_ping.sensor_data.gsfEchotracSpecific.navigation_error = 2;
      rec.sb_ping.sensor_data.gsfEchotracSpecific.mpp_source = 1;
      rec.sb_ping.sensor_data.gsfEchotracSpecific.tide_source = 3;
      ASSERT_GT(gsfWrite(h, &id, &rec), 0);
    }

    // 4. Sound velocity profile (SVP).
    {
      std::array<double, 3> svp_d = {0.0, 50.0, 150.0};
      std::array<double, 3> svp_v = {1500.0, 1495.5, 1490.0};
      gsfDataID id{};
      gsfRecords rec{};
      ZeroStructs(id, rec);
      id.recordID = GSF_RECORD_SOUND_VELOCITY_PROFILE;
      rec.svp.observation_time = {1700000002, 0};
      rec.svp.application_time = {1700000003, 0};
      rec.svp.latitude = 37.5;
      rec.svp.longitude = -122.25;
      rec.svp.number_points = 3;
      rec.svp.depth = svp_d.data();
      rec.svp.sound_speed = svp_v.data();
      ASSERT_GT(gsfWrite(h, &id, &rec), 0);
    }

    // 5. Processing parameters.
    {
      std::string p0 = "KEY0=VAL0";
      std::string p1 = "KEY1=VAL1";
      gsfDataID id{};
      gsfRecords rec{};
      ZeroStructs(id, rec);
      id.recordID = GSF_RECORD_PROCESSING_PARAMETERS;
      rec.process_parameters.param_time = {1700000004, 0};
      rec.process_parameters.number_parameters = 2;
      rec.process_parameters.param[0] = p0.data();
      rec.process_parameters.param_size[0] = static_cast<short>(p0.size());
      rec.process_parameters.param[1] = p1.data();
      rec.process_parameters.param_size[1] = static_cast<short>(p1.size());
      ASSERT_GT(gsfWrite(h, &id, &rec), 0);
    }

    // 6. Sensor parameters.
    {
      std::string s0 = "FREQ=400000";
      gsfDataID id{};
      gsfRecords rec{};
      ZeroStructs(id, rec);
      id.recordID = GSF_RECORD_SENSOR_PARAMETERS;
      rec.sensor_parameters.param_time = {1700000005, 0};
      rec.sensor_parameters.number_parameters = 1;
      rec.sensor_parameters.param[0] = s0.data();
      rec.sensor_parameters.param_size[0] = static_cast<short>(s0.size());
      ASSERT_GT(gsfWrite(h, &id, &rec), 0);
    }

    // 7. Comments exercising 0, 1, 2, 3 padding modulos (12 + len).
    for (std::string text : {"a", "ab", "abc", "abcd"}) {
      gsfDataID id{};
      gsfRecords rec{};
      ZeroStructs(id, rec);
      id.recordID = GSF_RECORD_COMMENT;
      rec.comment.comment_time = {1700000006, 0};
      rec.comment.comment_length = static_cast<int>(text.size());
      rec.comment.comment = text.data();
      ASSERT_GT(gsfWrite(h, &id, &rec), 0);
    }

    // 8. History.
    {
      std::string cmd = "mbproc --run";
      std::string hcomment = "clean-history";
      gsfDataID id{};
      gsfRecords rec{};
      ZeroStructs(id, rec);
      id.recordID = GSF_RECORD_HISTORY;
      rec.history.history_time = {1700000007, 0};
      CopyToFixedChars(rec.history.host_name, "vessel-host");
      CopyToFixedChars(rec.history.operator_name, "surveyor");
      rec.history.command_line = cmd.data();
      rec.history.comment = hcomment.data();
      ASSERT_GT(gsfWrite(h, &id, &rec), 0);
    }

    // 9. Legacy navigation error.
    {
      gsfDataID id{};
      gsfRecords rec{};
      ZeroStructs(id, rec);
      id.recordID = GSF_RECORD_NAVIGATION_ERROR;
      rec.nav_error.nav_error_time = {1700000008, 0};
      rec.nav_error.record_id = GSF_RECORD_SWATH_BATHYMETRY_PING;
      rec.nav_error.latitude_error = 1.2;
      rec.nav_error.longitude_error = 2.3;
      ASSERT_GT(gsfWrite(h, &id, &rec), 0);
    }

    // 10. HV navigation error.
    {
      std::string pos_type = GSF_POS_TYPE_PPSD;
      gsfDataID id{};
      gsfRecords rec{};
      ZeroStructs(id, rec);
      id.recordID = GSF_RECORD_HV_NAVIGATION_ERROR;
      rec.hv_nav_error.nav_error_time = {1700000009, 0};
      rec.hv_nav_error.record_id = GSF_RECORD_SWATH_BATHYMETRY_PING;
      rec.hv_nav_error.horizontal_error = 0.75;
      rec.hv_nav_error.vertical_error = 0.45;
      rec.hv_nav_error.SEP_uncertainty = 0.25;
      rec.hv_nav_error.spare[0] = 1;
      rec.hv_nav_error.spare[1] = 2;
      rec.hv_nav_error.position_type = pos_type.data();
      ASSERT_GT(gsfWrite(h, &id, &rec), 0);
    }

    // 11. Attitude record (last record in file).
    {
      std::array<struct timespec, 2> times = {
          {{1700000010, 0}, {1700000010, 500000000}}};
      std::array<double, 2> pitch = {1.25, 1.50};
      std::array<double, 2> roll = {-0.75, -0.50};
      std::array<double, 2> heave = {0.10, 0.12};
      std::array<double, 2> heading = {90.0, 91.0};
      gsfDataID id{};
      gsfRecords rec{};
      ZeroStructs(id, rec);
      id.recordID = GSF_RECORD_ATTITUDE;
      rec.attitude.num_measurements = 2;
      rec.attitude.attitude_time = times.data();
      rec.attitude.pitch = pitch.data();
      rec.attitude.roll = roll.data();
      rec.attitude.heave = heave.data();
      rec.attitude.heading = heading.data();
      ASSERT_GT(gsfWrite(h, &id, &rec), 0);
    }
  }

  // Reopen read-only and verify all 12 record types & beam arrays round-trip.
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_READONLY, &h));
  absl::Cleanup close_ro = [h] { CloseAndResetBrb(h); };

  gsfDataID id{};
  gsfRecords rec{};
  ZeroStructs(id, rec);

  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(GSF_RECORD_SWATH_BATHY_SUMMARY, id.recordID);
  // gsfUnpackStream stores raw (did & 0x80000000) in checksumFlag (non-zero).
  EXPECT_NE(0, id.checksumFlag);
  EXPECT_EQ(42, id.reserved);
  EXPECT_NEAR(205.5, rec.summary.max_depth, 0.05);

  {
    ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
    // gsfRecords is a struct (`gsf.h:1881`), and sequential decode never
    // updates `ft->rec.mb_ping.number_beams` (`gsf_dec.c:1869`), so `gsfFree`
    // at `gsf.c:2118` loops `[0..0)` and would orphan `time_series[i].samples`;
    // free `time_series[i].samples` explicitly here.
    gsfBRBIntensity* const brb = rec.mb_ping.brb_inten;
    const int beams = rec.mb_ping.number_beams;
    absl::Cleanup free_brb_samples = [brb, beams] {
      if (brb != nullptr && brb->time_series != nullptr) {
        for (int i = 0; i < beams; ++i) {
          std::free(brb->time_series[i].samples);
          brb->time_series[i].samples = nullptr;
        }
      }
    };
    EXPECT_EQ(GSF_RECORD_SWATH_BATHYMETRY_PING, id.recordID);
    EXPECT_NE(0, id.checksumFlag);
    EXPECT_EQ(7, id.reserved);
    ASSERT_EQ(4, rec.mb_ping.number_beams);
    ASSERT_NE(nullptr, rec.mb_ping.depth);
    EXPECT_NEAR(100.0, rec.mb_ping.depth[0], 0.02);
    EXPECT_NEAR(103.75, rec.mb_ping.depth[3], 0.02);
    ASSERT_NE(nullptr, rec.mb_ping.TVG_dB);
    EXPECT_NEAR(27.5, rec.mb_ping.TVG_dB[3], 0.2);
    ASSERT_NE(nullptr, rec.mb_ping.quality_flags);
    EXPECT_THAT(std::vector<unsigned char>(rec.mb_ping.quality_flags,
                                           rec.mb_ping.quality_flags + 4),
                ElementsAre(0, 1, 2, 3));
    ASSERT_NE(nullptr, rec.mb_ping.brb_inten);
    ASSERT_NE(nullptr, rec.mb_ping.brb_inten->time_series);
    EXPECT_EQ(16, rec.mb_ping.brb_inten->bits_per_sample);
    EXPECT_EQ(3, rec.mb_ping.brb_inten->time_series[0].sample_count);
    ASSERT_NE(nullptr, rec.mb_ping.brb_inten->time_series[0].samples);
    EXPECT_EQ(100u, rec.mb_ping.brb_inten->time_series[0].samples[0]);
  }

  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(GSF_RECORD_SINGLE_BEAM_PING, id.recordID);
  EXPECT_NEAR(45.25, rec.sb_ping.depth, 0.02);

  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(GSF_RECORD_SOUND_VELOCITY_PROFILE, id.recordID);
  EXPECT_EQ(3, rec.svp.number_points);
  ASSERT_NE(nullptr, rec.svp.sound_speed);
  EXPECT_NEAR(1495.5, rec.svp.sound_speed[1], 0.1);

  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(GSF_RECORD_PROCESSING_PARAMETERS, id.recordID);
  ASSERT_EQ(2, rec.process_parameters.number_parameters);
  EXPECT_STREQ("KEY0=VAL0", rec.process_parameters.param[0]);

  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(GSF_RECORD_SENSOR_PARAMETERS, id.recordID);
  ASSERT_EQ(1, rec.sensor_parameters.number_parameters);
  EXPECT_STREQ("FREQ=400000", rec.sensor_parameters.param[0]);

  for (std::string_view expected : {"a", "ab", "abc", "abcd"}) {
    ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
    EXPECT_EQ(GSF_RECORD_COMMENT, id.recordID);
    EXPECT_EQ(std::string(expected),
              std::string(rec.comment.comment, rec.comment.comment_length));
  }

  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(GSF_RECORD_HISTORY, id.recordID);
  EXPECT_STREQ("vessel-host", rec.history.host_name);
  EXPECT_STREQ("surveyor", rec.history.operator_name);
  EXPECT_STREQ("mbproc --run", rec.history.command_line);
  EXPECT_STREQ("clean-history", rec.history.comment);

  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(GSF_RECORD_NAVIGATION_ERROR, id.recordID);
  EXPECT_NEAR(1.2, rec.nav_error.latitude_error, 0.15);

  // Read HV_NAVIGATION_ERROR with caller stream buffer (buf != nullptr).
  std::vector<uint8_t> raw_copy(4096, 0xA5);
  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, raw_copy.data(),
                    static_cast<int>(raw_copy.size())),
            0);
  EXPECT_EQ(GSF_RECORD_HV_NAVIGATION_ERROR, id.recordID);
  EXPECT_STREQ(GSF_POS_TYPE_PPSD, rec.hv_nav_error.position_type);
  // First 4 bytes are big-endian tv_sec = 1700000009 (0x6553F109); tail intact.
  EXPECT_THAT(std::span<const uint8_t>(raw_copy).first<4>(),
              ElementsAre(0x65, 0x53, 0xF1, 0x09));
  EXPECT_EQ(0xA5u, raw_copy.back());

  // Reading Attitude with max_size == 1 triggers GSF_INSUFFICIENT_SIZE, then
  // gsfSeek(GSF_REWIND) + desiredRecord == GSF_RECORD_ATTITUDE exercises the
  // non-matching record skip loop at gsf.c:1066-1068.
  EXPECT_EQ(-1, gsfRead(h, GSF_NEXT_RECORD, &id, &rec, raw_copy.data(), 1));
  EXPECT_EQ(GSF_INSUFFICIENT_SIZE, gsfIntError());

  ASSERT_EQ(GSF_NORMAL, gsfSeek(h, GSF_REWIND));
  ASSERT_GT(gsfRead(h, GSF_RECORD_ATTITUDE, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(GSF_RECORD_ATTITUDE, id.recordID);
  EXPECT_EQ(2, rec.attitude.num_measurements);
  ASSERT_NE(nullptr, rec.attitude.heading);
  EXPECT_NEAR(91.0, rec.attitude.heading[1], 0.05);
}

TEST_F(GsfTest, WriteAndReadHandleInvalidArgumentsAndCorruptStreams) {
  // 1. gsfWrite and gsfRead handle validation and encode failures.
  gsfDataID bad_id{};
  gsfRecords bad_rec{};
  ZeroStructs(bad_id, bad_rec);
  EXPECT_EQ(-1, gsfWrite(kBadHandleZero, &bad_id, &bad_rec));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());
  EXPECT_EQ(-1, gsfWrite(kBadHandleOver, &bad_id, &bad_rec));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());
  EXPECT_EQ(-1, gsfRead(kBadHandleZero, GSF_NEXT_RECORD, &bad_id, &bad_rec,
                        nullptr, 0));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());
  EXPECT_EQ(-1, gsfRead(kBadHandleOver, GSF_NEXT_RECORD, &bad_id, &bad_rec,
                        nullptr, 0));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());

  const std::filesystem::path werr_path = TestPath("write_errors.gsf");
  int h = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(werr_path.c_str(), GSF_CREATE, &h));
  {
    absl::Cleanup close_h = [h] { gsfClose(h); };
    bad_id.recordID = 99;
    EXPECT_EQ(-1, gsfWrite(h, &bad_id, &bad_rec));
    EXPECT_EQ(GSF_UNRECOGNIZED_RECORD_ID, gsfIntError());

    bad_id.recordID = GSF_RECORD_SWATH_BATHYMETRY_PING;
    bad_rec.mb_ping.number_beams = 0;
    EXPECT_EQ(-1, gsfWrite(h, &bad_id, &bad_rec));
    EXPECT_EQ(GSF_INVALID_NUM_BEAMS, gsfIntError());

    bad_id.recordID = GSF_RECORD_SINGLE_BEAM_PING;
    bad_rec.sb_ping.sensor_id = 999;
    EXPECT_EQ(-1, gsfWrite(h, &bad_id, &bad_rec));
    EXPECT_EQ(GSF_SINGLE_BEAM_ENCODE_FAILED, gsfIntError());
  }

  // Attempting gsfWrite on a GSF_READONLY handle fails with GSF_WRITE_ERROR.
  ASSERT_EQ(GSF_NORMAL, gsfOpen(werr_path.c_str(), GSF_READONLY, &h));
  {
    absl::Cleanup close_h = [h] { gsfClose(h); };
    std::string txt = "ro";
    bad_id.recordID = GSF_RECORD_COMMENT;
    bad_rec.comment.comment_length = static_cast<int>(txt.size());
    bad_rec.comment.comment = txt.data();
    EXPECT_EQ(-1, gsfWrite(h, &bad_id, &bad_rec));
    EXPECT_EQ(GSF_WRITE_ERROR, gsfIntError());
  }

  // 2. Table of corrupt framed files triggering gsfUnpackStream / decode errors.
  struct CorruptCase {
    std::string_view name;
    int expected_error;
    std::vector<uint8_t> suffix;
  };
  const std::vector<uint8_t> valid_header = MakeHeaderPayload(GSF_VERSION);

  auto make_bad_size = [](uint32_t sz, uint32_t rec_id) {
    std::vector<uint8_t> v;
    AppendBe32(v, sz);
    AppendBe32(v, rec_id);
    return v;
  };
  auto make_bad_checksum = []() {
    std::vector<uint8_t> v;
    AppendRawRecord(v, /*checksum_flag=*/true, 0, GSF_RECORD_COMMENT,
                    std::vector<uint8_t>(16, 0), /*corrupt_checksum=*/true);
    return v;
  };
  auto make_zero_record = [](uint32_t rec_id, size_t payload_len) {
    std::vector<uint8_t> v;
    AppendRawRecord(v, /*checksum_flag=*/false, 0, rec_id,
                    std::vector<uint8_t>(payload_len, 0));
    return v;
  };
  auto make_bad_single_beam = []() {
    std::vector<uint8_t> payload(44, 0);
    // Byte offset 38 begins subrecords: unrecognized ID 250 with size 10 > 2
    // remaining bytes forces gsfDecodeSinglebeam to return -1.
    payload[38] = 250u;
    payload[39] = 0u;
    payload[40] = 0u;
    payload[41] = 10u;
    std::vector<uint8_t> v;
    AppendRawRecord(v, false, 0, GSF_RECORD_SINGLE_BEAM_PING, payload);
    return v;
  };

  const std::vector<CorruptCase> cases = {
      {"size_too_small.gsf", GSF_RECORD_SIZE_ERROR,
       make_bad_size(4, GSF_RECORD_COMMENT)},
      {"size_too_large.gsf", GSF_RECORD_SIZE_ERROR,
       make_bad_size(GSF_MAX_RECORD_SIZE + 4, GSF_RECORD_COMMENT)},
      {"bad_rec_id.gsf", GSF_UNRECOGNIZED_RECORD_ID,
       make_bad_size(12, 99u)},
      {"bad_checksum.gsf", GSF_CHECKSUM_FAILURE, make_bad_checksum()},
      {"eof_2bytes.gsf", GSF_READ_TO_END_OF_FILE, {0x01, 0x02}},
      {"partial_5bytes.gsf",
       GSF_PARTIAL_RECORD_AT_END_OF_FILE,
       {0x00, 0x00, 0x00, 0x10, 0x00}},
      {"eof_empty_payload.gsf",
       GSF_READ_TO_END_OF_FILE,
       make_bad_size(16, GSF_RECORD_COMMENT)},
      {"partial_payload.gsf",
       GSF_PARTIAL_RECORD_AT_END_OF_FILE,
       []() {
         std::vector<uint8_t> v;
         AppendBe32(v, 16);
         AppendBe32(v, GSF_RECORD_COMMENT);
         v.insert(v.end(), {1, 2, 3, 4});
         return v;
       }()},
      {"bad_ping_beams.gsf", GSF_INVALID_NUM_BEAMS,
       make_zero_record(GSF_RECORD_SWATH_BATHYMETRY_PING, 20)},
      {"bad_history.gsf", GSF_HISTORY_RECORD_DECODE_FAILED,
       make_zero_record(GSF_RECORD_HISTORY, 12)},
      {"bad_attitude.gsf", GSF_ATTITUDE_RECORD_DECODE_FAILED,
       make_zero_record(GSF_RECORD_ATTITUDE, 12)},
      {"bad_single_beam.gsf", GSF_UNRECOGNIZED_SUBRECORD_ID,
       make_bad_single_beam()},
  };

  for (const CorruptCase& tc : cases) {
    const std::filesystem::path cpath = TestPath(tc.name);
    std::vector<uint8_t> bytes;
    AppendRawRecord(bytes, false, 0, GSF_RECORD_HEADER, valid_header);
    bytes.insert(bytes.end(), tc.suffix.begin(), tc.suffix.end());
    WriteBytesToFile(cpath, bytes);

    int hc = 0;
    ASSERT_EQ(GSF_NORMAL, gsfOpen(cpath.c_str(), GSF_READONLY, &hc))
        << tc.name;
    absl::Cleanup close_hc = [hc] { gsfClose(hc); };
    gsfDataID cid{};
    gsfRecords crec{};
    ZeroStructs(cid, crec);
    EXPECT_EQ(-1, gsfRead(hc, GSF_NEXT_RECORD, &cid, &crec, nullptr, 0))
        << tc.name;
    EXPECT_EQ(tc.expected_error, gsfIntError()) << tc.name;
  }
}

// --- Group C: Direct Access (gsfSeekRecord, gsfIndexTime, gsfGetNumberRecords)

TEST_F(GsfTest, DirectAccessIndexReadsTimeCountsUpdatesAndErrors) {
  const std::filesystem::path path = TestPath("indexed.gsf");

  // 1. Write 2 comments and 3 pings where ping 2 switches scale factors
  // (generating 2 scale_factor_addr entries in the .nsf index).
  int h = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_CREATE, &h));
  {
    absl::Cleanup close_h = [h] { gsfClose(h); };
    auto write_comment = [h](std::string text, time_t sec) {
      gsfDataID id{};
      gsfRecords rec{};
      ZeroStructs(id, rec);
      id.recordID = GSF_RECORD_COMMENT;
      rec.comment.comment_time = {sec, 10};
      rec.comment.comment_length = static_cast<int>(text.size());
      rec.comment.comment = text.data();
      ASSERT_GT(gsfWrite(h, &id, &rec), 0);
    };
    auto write_ping = [h](time_t sec, long nsec, double depth_val,
                          double precision) {
      std::array<double, 2> depths = {depth_val, depth_val + 1.0};
      gsfDataID id{};
      gsfRecords rec{};
      ZeroStructs(id, rec);
      id.recordID = GSF_RECORD_SWATH_BATHYMETRY_PING;
      rec.mb_ping.ping_time = {sec, nsec};
      rec.mb_ping.number_beams = 2;
      rec.mb_ping.depth = depths.data();
      // Non-empty sensor subrecord avoids gsf_dec.c:1891 reading uninitialized
      // `records.mb_ping.sensor_id` inside gsfCreateIndexFile (`gsf_indx.c:516`).
      rec.mb_ping.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC;
      ASSERT_EQ(GSF_NORMAL,
                gsfLoadScaleFactor(&rec.mb_ping.scaleFactors,
                                   GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                                   GSF_FIELD_SIZE_TWO, precision, 0));
      ASSERT_GT(gsfWrite(h, &id, &rec), 0);
    };

    write_comment("alpha", 900);
    write_ping(1000, 100, 10.0, 0.01);
    write_ping(1010, 200, 20.0, 0.001);
    write_ping(1020, 300, 30.0, 0.001);
    write_comment("bravo", 1030);
  }

  // 2. Non-indexed handle rejects gsfGetNumberRecords with GSF_BAD_ACCESS_MODE.
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_READONLY, &h));
  {
    absl::Cleanup close_h = [h] { gsfClose(h); };
    EXPECT_EQ(-1, gsfGetNumberRecords(h, GSF_RECORD_COMMENT));
    EXPECT_EQ(GSF_BAD_ACCESS_MODE, gsfIntError());
  }

  // 3. Build `.nsf` first and flush gsfFileTable[0].rec (`gsfCreateIndexFile`
  // leaves the last read comment allocated while subsequent `gsfSeekRecord`
  // overwrites ft->rec via memcpy at gsf.c:1387), then reopen GSF_READONLY_INDEX
  // to validate record counts, times, and seeks.
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_READONLY_INDEX, &h));
  ASSERT_EQ(GSF_NORMAL, gsfClose(h));
  auto flush_slot = [this](std::string_view name) {
    int fh = 0;
    const std::filesystem::path flush_path = TestPath(name);
    if (gsfOpen(flush_path.c_str(), GSF_CREATE, &fh) == GSF_NORMAL) {
      gsfClose(fh);
    }
  };
  flush_slot("flush_direct_idx_slot.gsf");

  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_READONLY_INDEX, &h));
  {
    absl::Cleanup close_h = [h] { gsfClose(h); };

    EXPECT_EQ(-1, gsfGetNumberRecords(kBadHandleZero, GSF_RECORD_COMMENT));
    EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());
    EXPECT_EQ(-1, gsfGetNumberRecords(kBadHandleOver, GSF_RECORD_COMMENT));
    EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());
    EXPECT_EQ(-1, gsfGetNumberRecords(h, -1));
    EXPECT_EQ(GSF_UNRECOGNIZED_RECORD_ID, gsfIntError());
    EXPECT_EQ(-1, gsfGetNumberRecords(h, NUM_REC_TYPES));
    EXPECT_EQ(GSF_UNRECOGNIZED_RECORD_ID, gsfIntError());

    EXPECT_EQ(2, gsfGetNumberRecords(h, GSF_NEXT_RECORD));
    EXPECT_EQ(3, gsfGetNumberRecords(h, GSF_RECORD_SWATH_BATHYMETRY_PING));
    EXPECT_EQ(2, gsfGetNumberRecords(h, GSF_RECORD_COMMENT));
    EXPECT_EQ(0, gsfGetNumberRecords(h, GSF_RECORD_ATTITUDE));

    time_t sec = 0;
    long nsec = 0;
    EXPECT_EQ(-1, gsfIndexTime(kBadHandleZero, GSF_RECORD_SWATH_BATHYMETRY_PING,
                               1, &sec, &nsec));
    EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());
    EXPECT_EQ(-1, gsfIndexTime(kBadHandleOver, GSF_RECORD_SWATH_BATHYMETRY_PING,
                               1, &sec, &nsec));
    EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());
    EXPECT_EQ(-1, gsfIndexTime(h, 0, 1, &sec, &nsec));
    EXPECT_EQ(GSF_UNRECOGNIZED_RECORD_ID, gsfIntError());
    EXPECT_EQ(-1, gsfIndexTime(h, NUM_REC_TYPES, 1, &sec, &nsec));
    EXPECT_EQ(GSF_UNRECOGNIZED_RECORD_ID, gsfIntError());
    EXPECT_EQ(-1, gsfIndexTime(h, GSF_RECORD_SWATH_BATHYMETRY_PING, 0, &sec,
                               &nsec));
    EXPECT_EQ(GSF_INVALID_RECORD_NUMBER, gsfIntError());
    EXPECT_EQ(-1, gsfIndexTime(h, GSF_RECORD_SWATH_BATHYMETRY_PING, 4, &sec,
                               &nsec));
    EXPECT_EQ(GSF_INVALID_RECORD_NUMBER, gsfIntError());
    EXPECT_EQ(-1, gsfIndexTime(h, GSF_RECORD_ATTITUDE, -1, &sec, &nsec));
    EXPECT_EQ(GSF_RECORD_TYPE_NOT_AVAILABLE, gsfIntError());

    EXPECT_EQ(2, gsfIndexTime(h, GSF_RECORD_SWATH_BATHYMETRY_PING, 2, &sec,
                              &nsec));
    EXPECT_EQ(1010, sec);
    EXPECT_EQ(200L, nsec);
    EXPECT_EQ(3, gsfIndexTime(h, GSF_RECORD_SWATH_BATHYMETRY_PING, -1, &sec,
                              &nsec));
    EXPECT_EQ(1020, sec);
    EXPECT_EQ(300L, nsec);

    // Read pings before comments because gsfSeekRecord memcpy-overwrites
    // gsfFileTable[h-1].rec when reloading scale factors (gsf.c:1387).
    gsfDataID id{};
    gsfRecords rec{};
    ZeroStructs(id, rec);
    id.record_number = 3;
    ASSERT_GT(gsfRead(h, GSF_RECORD_SWATH_BATHYMETRY_PING, &id, &rec, nullptr,
                      0),
              0);
    EXPECT_NEAR(30.0, rec.mb_ping.depth[0], 0.01);

    unsigned char cflag = 0;
    double mult = 0.0;
    double offset = 0.0;
    ASSERT_EQ(GSF_NORMAL,
              gsfGetScaleFactor(h, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                                &cflag, &mult, &offset));
    EXPECT_DOUBLE_EQ(1000.0, mult);

    id.record_number = 1;
    ASSERT_GT(gsfRead(h, GSF_RECORD_SWATH_BATHYMETRY_PING, &id, &rec, nullptr,
                      0),
              0);
    EXPECT_NEAR(10.0, rec.mb_ping.depth[0], 0.02);
    ASSERT_EQ(GSF_NORMAL,
              gsfGetScaleFactor(h, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                                &cflag, &mult, &offset));
    EXPECT_DOUBLE_EQ(100.0, mult);

    id.record_number = -1;
    ASSERT_GT(gsfRead(h, GSF_RECORD_SWATH_BATHYMETRY_PING, &id, &rec, nullptr,
                      0),
              0);
    EXPECT_NEAR(30.0, rec.mb_ping.depth[0], 0.01);

    id.record_number = 1;
    ASSERT_GT(gsfRead(h, GSF_RECORD_COMMENT, &id, &rec, nullptr, 0), 0);
    EXPECT_STREQ("alpha", rec.comment.comment);
    id.record_number = -1;
    ASSERT_GT(gsfRead(h, GSF_RECORD_COMMENT, &id, &rec, nullptr, 0), 0);
    EXPECT_STREQ("bravo", rec.comment.comment);

    id.record_number = 0;
    EXPECT_EQ(-1, gsfRead(h, GSF_RECORD_HEADER, &id, &rec, nullptr, 0));
    EXPECT_EQ(GSF_INVALID_RECORD_NUMBER, gsfIntError());
    id.record_number = -1;
    EXPECT_EQ(-1, gsfRead(h, GSF_RECORD_ATTITUDE, &id, &rec, nullptr, 0));
    EXPECT_EQ(GSF_RECORD_TYPE_NOT_AVAILABLE, gsfIntError());
    id.record_number = 1;
    EXPECT_EQ(-1, gsfRead(h, 99, &id, &rec, nullptr, 0));
    EXPECT_EQ(GSF_UNRECOGNIZED_RECORD_ID, gsfIntError());
  }

  // 4. Open GSF_UPDATE_INDEX, overwrite comment 1 in-place with identical size.
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_UPDATE_INDEX, &h));
  {
    absl::Cleanup close_h = [h] { gsfClose(h); };
    std::string replacement = "omega";
    gsfDataID id{};
    gsfRecords rec{};
    ZeroStructs(id, rec);
    id.recordID = GSF_RECORD_COMMENT;
    id.record_number = 1;
    rec.comment.comment_time = {905, 0};
    rec.comment.comment_length = static_cast<int>(replacement.size());
    rec.comment.comment = replacement.data();
    ASSERT_GT(gsfWrite(h, &id, &rec), 0);

    id.record_number = 99;
    EXPECT_EQ(-1, gsfWrite(h, &id, &rec));
    EXPECT_EQ(GSF_INVALID_RECORD_NUMBER, gsfIntError());
  }

  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_READONLY_INDEX, &h));
  absl::Cleanup close_verify = [h] { gsfClose(h); };
  gsfDataID id{};
  gsfRecords rec{};
  ZeroStructs(id, rec);
  id.record_number = 1;
  ASSERT_GT(gsfRead(h, GSF_RECORD_COMMENT, &id, &rec, nullptr, 0), 0);
  EXPECT_STREQ("omega", rec.comment.comment);
  id.record_number = 2;
  ASSERT_GT(gsfRead(h, GSF_RECORD_COMMENT, &id, &rec, nullptr, 0), 0);
  EXPECT_STREQ("bravo", rec.comment.comment);
}

// --- Group D: Scale Factors & Array Min/Max ---

TEST_F(GsfTest, LoadAndGetScaleFactorValidAndErrors) {
  gsfScaleFactors sf{};
  ZeroStructs(sf);
  EXPECT_EQ(-1, gsfLoadScaleFactor(&sf, 0, GSF_FIELD_SIZE_DEFAULT, 0.01, 0));
  EXPECT_EQ(GSF_UNRECOGNIZED_ARRAY_SUBRECORD_ID, gsfIntError());
  EXPECT_EQ(-1, gsfLoadScaleFactor(&sf, GSF_MAX_PING_ARRAY_SUBRECORDS + 1,
                                   GSF_FIELD_SIZE_DEFAULT, 0.01, 0));
  EXPECT_EQ(GSF_UNRECOGNIZED_ARRAY_SUBRECORD_ID, gsfIntError());

  EXPECT_EQ(-1, gsfLoadScaleFactor(&sf, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                                   GSF_FIELD_SIZE_DEFAULT, 0.0, 0));
  EXPECT_EQ(GSF_CANNOT_REPRESENT_PRECISION, gsfIntError());
  EXPECT_EQ(-1, gsfLoadScaleFactor(&sf, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                                   GSF_FIELD_SIZE_DEFAULT, -0.5, 0));
  EXPECT_EQ(GSF_CANNOT_REPRESENT_PRECISION, gsfIntError());
  // precision == 2.0 truncates (1.0 / 2.0) + 0.001 to 0 < MIN_GSF_SF_MULT_VALUE
  // cleanly without float-to-int overflow.
  EXPECT_EQ(-1, gsfLoadScaleFactor(&sf, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                                   GSF_FIELD_SIZE_DEFAULT, 2.0, 0));
  EXPECT_EQ(GSF_CANNOT_REPRESENT_PRECISION, gsfIntError());

  // Populate all 31 slots and update slot 1 (hits existing-multiplier branch).
  for (unsigned int id = 1; id <= GSF_MAX_PING_ARRAY_SUBRECORDS; ++id) {
    ASSERT_EQ(GSF_NORMAL,
              gsfLoadScaleFactor(&sf, id, GSF_FIELD_SIZE_DEFAULT, 0.01, 0));
  }
  EXPECT_EQ(GSF_MAX_PING_ARRAY_SUBRECORDS, sf.numArraySubrecords);

  ASSERT_EQ(GSF_NORMAL,
            gsfLoadScaleFactor(&sf, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                               GSF_FIELD_SIZE_FOUR, 0.001, 25));
  EXPECT_EQ(GSF_MAX_PING_ARRAY_SUBRECORDS, sf.numArraySubrecords);
  EXPECT_EQ(GSF_FIELD_SIZE_FOUR,
            sf.scaleTable[GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY - 1]
                .compressionFlag);
  EXPECT_DOUBLE_EQ(1000.0,
                   sf.scaleTable[GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY - 1]
                       .multiplier);
  EXPECT_DOUBLE_EQ(25.0,
                   sf.scaleTable[GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY - 1]
                       .offset);

  // Trigger GSF_TOO_MANY_ARRAY_SUBRECORDS when numArraySubrecords == 31 and a
  // zeroed slot is loaded (gsf.c:1790).
  sf.scaleTable[0].multiplier = 0.0;
  EXPECT_EQ(-1, gsfLoadScaleFactor(&sf, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                                   GSF_FIELD_SIZE_DEFAULT, 0.01, 0));
  EXPECT_EQ(GSF_TOO_MANY_ARRAY_SUBRECORDS, gsfIntError());
  EXPECT_EQ(30, sf.numArraySubrecords);

  // Test existing-multiplier invalid precision path (gsf.c:1830).
  EXPECT_EQ(-1, gsfLoadScaleFactor(&sf, GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY,
                                   GSF_FIELD_SIZE_DEFAULT, 2.0, 0));
  EXPECT_EQ(GSF_CANNOT_REPRESENT_PRECISION, gsfIntError());

  // Test gsfGetScaleFactor error paths and valid round-trip.
  unsigned char cflag = 0;
  double mult = 0.0;
  double offset = 0.0;
  EXPECT_EQ(-1, gsfGetScaleFactor(1, 0, &cflag, &mult, &offset));
  EXPECT_EQ(GSF_UNRECOGNIZED_ARRAY_SUBRECORD_ID, gsfIntError());
  EXPECT_EQ(-1, gsfGetScaleFactor(1, GSF_MAX_PING_ARRAY_SUBRECORDS + 1, &cflag,
                                  &mult, &offset));
  EXPECT_EQ(GSF_UNRECOGNIZED_ARRAY_SUBRECORD_ID, gsfIntError());
  EXPECT_EQ(-1, gsfGetScaleFactor(kBadHandleZero,
                                  GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY, &cflag,
                                  &mult, &offset));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());
  EXPECT_EQ(-1, gsfGetScaleFactor(kBadHandleOver,
                                  GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY, &cflag,
                                  &mult, &offset));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());

  const std::filesystem::path path = TestPath("scale_factors.gsf");
  int h = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_CREATE, &h));
  absl::Cleanup close_h = [h] { gsfClose(h); };

  std::array<double, 2> depths = {50.0, 55.0};
  gsfDataID id{};
  gsfRecords rec{};
  gsfRecords read_rec{};
  ZeroStructs(id, rec, read_rec);
  id.recordID = GSF_RECORD_SWATH_BATHYMETRY_PING;
  rec.mb_ping.number_beams = 2;
  rec.mb_ping.depth = depths.data();
  ASSERT_EQ(GSF_NORMAL,
            gsfLoadScaleFactor(&rec.mb_ping.scaleFactors,
                               GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                               GSF_FIELD_SIZE_FOUR, 0.01, 10));
  ASSERT_GT(gsfWrite(h, &id, &rec), 0);
  ASSERT_EQ(GSF_NORMAL, gsfSeek(h, GSF_REWIND));
  ASSERT_GT(gsfRead(h, GSF_RECORD_SWATH_BATHYMETRY_PING, &id, &read_rec,
                    nullptr, 0),
            0);

  ASSERT_EQ(GSF_NORMAL,
            gsfGetScaleFactor(h, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY, &cflag,
                              &mult, &offset));
  EXPECT_EQ(GSF_FIELD_SIZE_FOUR, cflag);
  EXPECT_DOUBLE_EQ(100.0, mult);
  EXPECT_DOUBLE_EQ(10.0, offset);

  EXPECT_EQ(-1, gsfGetScaleFactor(h, GSF_SWATH_BATHY_SUBRECORD_TVG_ARRAY,
                                  &cflag, &mult, &offset));
  EXPECT_EQ(GSF_ILLEGAL_SCALE_FACTOR_MULTIPLIER, gsfIntError());
}

TEST_F(GsfTest, LoadDepthScaleFactorAutoOffsetTransitionsAndErrors) {
  gsfSwathBathyPing ping{};
  ZeroStructs(ping);
  double last_corrector = 0.0;

  EXPECT_EQ(-1, gsfLoadDepthScaleFactorAutoOffset(
                    &ping, GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY,
                    /*reset=*/1, 0.0, 50.0, &last_corrector,
                    GSF_FIELD_SIZE_DEFAULT, 0.01));
  EXPECT_EQ(GSF_UNRECOGNIZED_ARRAY_SUBRECORD_ID, gsfIntError());

  EXPECT_EQ(-1, gsfLoadDepthScaleFactorAutoOffset(
                    &ping, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY, /*reset=*/1,
                    0.0, 50.0, &last_corrector, GSF_FIELD_SIZE_DEFAULT, 0.0));
  EXPECT_EQ(GSF_CANNOT_REPRESENT_PRECISION, gsfIntError());

  // 1. Reset with corrector in [0, 100): layer=0 < 100 -> offset clamped to 20.
  ping.ping_flags = GSF_PING_USER_FLAG_14;
  ping.depth_corrector = 10.0;
  ping.tide_corrector = 5.0;
  ASSERT_EQ(GSF_NORMAL, gsfLoadDepthScaleFactorAutoOffset(
                            &ping, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                            /*reset=*/1, 0.0, 50.0, &last_corrector,
                            GSF_FIELD_SIZE_TWO, 0.01));
  EXPECT_DOUBLE_EQ(15.0, last_corrector);
  EXPECT_DOUBLE_EQ(
      20.0,
      ping.scaleFactors.scaleTable[GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY - 1]
          .offset);

  // 2. Increasing corrector passing 30% threshold sets offset = -200.
  ping.ping_flags = GSF_PING_USER_FLAG_13;
  ping.gps_tide_corrector = 250.0;
  ASSERT_EQ(GSF_NORMAL, gsfLoadDepthScaleFactorAutoOffset(
                            &ping, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                            /*reset=*/0, 0.0, 50.0, &last_corrector,
                            GSF_FIELD_SIZE_TWO, 0.01));
  EXPECT_DOUBLE_EQ(250.0, last_corrector);
  EXPECT_DOUBLE_EQ(
      -200.0,
      ping.scaleFactors.scaleTable[GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY - 1]
          .offset);

  // 3. Unchanged corrector preserves last_corrector without error.
  ASSERT_EQ(GSF_NORMAL, gsfLoadDepthScaleFactorAutoOffset(
                            &ping, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                            /*reset=*/0, 0.0, 50.0, &last_corrector,
                            GSF_FIELD_SIZE_TWO, 0.01));

  // 4. Decreasing negative fraction crossing -90 triggers shallow-layer shift
  // and clamps positive offset at 20.
  ping.gps_tide_corrector = -195.0;
  ASSERT_EQ(GSF_NORMAL, gsfLoadDepthScaleFactorAutoOffset(
                            &ping, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                            /*reset=*/0, 0.0, 50.0, &last_corrector,
                            GSF_FIELD_SIZE_TWO, 0.01));
  EXPECT_DOUBLE_EQ(-195.0, last_corrector);
  EXPECT_DOUBLE_EQ(
      20.0,
      ping.scaleFactors.scaleTable[GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY - 1]
          .offset);

  // 5. Reset with layer >= layer_interval + precision < 0.01 + deep > 430m.
  ping.ping_flags = GSF_PING_USER_FLAG_15;
  ping.depth_corrector = 120.0;
  ping.tide_corrector = 35.0;
  ASSERT_EQ(GSF_NORMAL,
            gsfLoadDepthScaleFactorAutoOffset(
                &ping, GSF_SWATH_BATHY_SUBRECORD_NOMINAL_DEPTH_ARRAY,
                /*reset=*/1, 0.0, 50.0, &last_corrector, GSF_FIELD_SIZE_TWO,
                0.001));
  EXPECT_DOUBLE_EQ(-150.0,
                   ping.scaleFactors
                       .scaleTable[GSF_SWATH_BATHY_SUBRECORD_NOMINAL_DEPTH_ARRAY -
                                   1]
                       .offset);

  ping.ping_flags = GSF_PING_USER_FLAG_13;
  ping.gps_tide_corrector = 50.0;
  last_corrector = 0.0;
  ASSERT_EQ(GSF_NORMAL, gsfLoadDepthScaleFactorAutoOffset(
                            &ping, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                            /*reset=*/0, 0.0, 500.0, &last_corrector,
                            GSF_FIELD_SIZE_TWO, 0.01));
  EXPECT_DOUBLE_EQ(
      0.0,
      ping.scaleFactors.scaleTable[GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY - 1]
          .offset);

  ping.scaleFactors.scaleTable[GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY - 1]
      .offset = 20;
  ping.gps_tide_corrector = 25.0;
  ASSERT_EQ(GSF_NORMAL, gsfLoadDepthScaleFactorAutoOffset(
                            &ping, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                            /*reset=*/0, 0.0, 500.0, &last_corrector,
                            GSF_FIELD_SIZE_TWO, 0.01));
  EXPECT_DOUBLE_EQ(
      0.0,
      ping.scaleFactors.scaleTable[GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY - 1]
          .offset);
}

TEST_F(GsfTest, GetSwathBathyArrayMinMaxCoversAllSubrecordsAndSizes) {
  gsfSwathBathyPing ping{};
  ZeroStructs(ping);
  double min_val = 0.0;
  double max_val = 0.0;

  // Uninitialized multiplier == 0.0 fails cleanly for valid subrecord [1..31].
  EXPECT_EQ(-1, gsfGetSwathBathyArrayMinMax(
                    &ping, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY, &min_val,
                    &max_val));
  EXPECT_EQ(GSF_ILLEGAL_SCALE_FACTOR_MULTIPLIER, gsfIntError());

  struct MinMaxCase {
    unsigned int subrecord_id;
    unsigned char field_size;
    double expected_min;
    double expected_max;
  };
  constexpr double kMult = 10.0;
  constexpr double kOffset = 5.0;
  constexpr auto kValidCases = std::to_array<MinMaxCase>({
      {GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY, GSF_FIELD_SIZE_ONE, -5.0, 20.5},
      {GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY, GSF_FIELD_SIZE_TWO, -5.0, 6548.5},
      {GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY, GSF_FIELD_SIZE_FOUR, -5.0,
       429496724.5},
      {GSF_SWATH_BATHY_SUBRECORD_NOMINAL_DEPTH_ARRAY, GSF_FIELD_SIZE_DEFAULT,
       -5.0, 6548.5},
      {GSF_SWATH_BATHY_SUBRECORD_TRAVEL_TIME_ARRAY, GSF_FIELD_SIZE_TWO, -5.0,
       6548.5},
      {GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY, GSF_FIELD_SIZE_ONE, -17.8,
       7.7},
      {GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY, GSF_FIELD_SIZE_TWO,
       -3281.8, 3271.7},
      {GSF_SWATH_BATHY_SUBRECORD_ALONG_TRACK_ARRAY, GSF_FIELD_SIZE_FOUR,
       -214748369.8, 214748359.7},
      {GSF_SWATH_BATHY_SUBRECORD_MEAN_CAL_AMPLITUDE_ARRAY, GSF_FIELD_SIZE_ONE,
       -17.8, 7.7},
      {GSF_SWATH_BATHY_SUBRECORD_MEAN_CAL_AMPLITUDE_ARRAY, GSF_FIELD_SIZE_TWO,
       -3281.8, 3271.7},
      {GSF_SWATH_BATHY_SUBRECORD_MEAN_REL_AMPLITUDE_ARRAY, GSF_FIELD_SIZE_ONE,
       -5.0, 20.5},
      {GSF_SWATH_BATHY_SUBRECORD_ECHO_WIDTH_ARRAY, GSF_FIELD_SIZE_TWO, -5.0,
       6548.5},
      {GSF_SWATH_BATHY_SUBRECORD_BEAM_ANGLE_ARRAY, GSF_FIELD_SIZE_DEFAULT,
       -3281.8, 3271.7},
      {GSF_SWATH_BATHY_SUBRECORD_DEPTH_ERROR_ARRAY, GSF_FIELD_SIZE_DEFAULT,
       -5.0, 6548.5},
      {GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ERROR_ARRAY,
       GSF_FIELD_SIZE_DEFAULT, -5.0, 6548.5},
      {GSF_SWATH_BATHY_SUBRECORD_ALONG_TRACK_ERROR_ARRAY,
       GSF_FIELD_SIZE_DEFAULT, -5.0, 6548.5},
      {GSF_SWATH_BATHY_SUBRECORD_BEAM_ANGLE_FORWARD_ARRAY,
       GSF_FIELD_SIZE_DEFAULT, -5.0, 6548.5},
      {GSF_SWATH_BATHY_SUBRECORD_VERTICAL_ERROR_ARRAY, GSF_FIELD_SIZE_DEFAULT,
       -5.0, 6548.5},
      {GSF_SWATH_BATHY_SUBRECORD_HORIZONTAL_ERROR_ARRAY,
       GSF_FIELD_SIZE_DEFAULT, -5.0, 6548.5},
      {GSF_SWATH_BATHY_SUBRECORD_QUALITY_FACTOR_ARRAY, GSF_FIELD_SIZE_DEFAULT,
       -5.0, 20.5},
      {GSF_SWATH_BATHY_SUBRECORD_QUALITY_FLAGS_ARRAY, GSF_FIELD_SIZE_DEFAULT,
       -5.0, 20.5},
      {GSF_SWATH_BATHY_SUBRECORD_BEAM_FLAGS_ARRAY, GSF_FIELD_SIZE_DEFAULT,
       -5.0, 20.5},
      {GSF_SWATH_BATHY_SUBRECORD_SIGNAL_TO_NOISE_ARRAY, GSF_FIELD_SIZE_DEFAULT,
       -5.0, 20.5},
      {GSF_SWATH_BATHY_SUBRECORD_SECTOR_NUMBER_ARRAY, GSF_FIELD_SIZE_DEFAULT,
       -5.0, 20.5},
      {GSF_SWATH_BATHY_SUBRECORD_DETECTION_INFO_ARRAY, GSF_FIELD_SIZE_DEFAULT,
       -5.0, 20.5},
      {GSF_SWATH_BATHY_SUBRECORD_SYSTEM_CLEANING_ARRAY, GSF_FIELD_SIZE_DEFAULT,
       -5.0, 20.5},
      {GSF_SWATH_BATHY_SUBRECORD_TVG_ARRAY, GSF_FIELD_SIZE_DEFAULT, -5.0,
       20.5},
      {GSF_SWATH_BATHY_SUBRECORD_RECEIVE_HEAVE_ARRAY, GSF_FIELD_SIZE_DEFAULT,
       -17.8, 7.7},
      {GSF_SWATH_BATHY_SUBRECORD_INCIDENT_BEAM_ADJ_ARRAY,
       GSF_FIELD_SIZE_DEFAULT, -17.8, 7.7},
      {GSF_SWATH_BATHY_SUBRECORD_DOPPLER_CORRECTION_ARRAY,
       GSF_FIELD_SIZE_DEFAULT, -17.8, 7.7},
  });

  for (const MinMaxCase& tc : kValidCases) {
    ping.scaleFactors.scaleTable[tc.subrecord_id - 1].compressionFlag =
        tc.field_size;
    ping.scaleFactors.scaleTable[tc.subrecord_id - 1].multiplier = kMult;
    ping.scaleFactors.scaleTable[tc.subrecord_id - 1].offset = kOffset;
    ASSERT_EQ(GSF_NORMAL, gsfGetSwathBathyArrayMinMax(&ping, tc.subrecord_id,
                                                      &min_val, &max_val))
        << "subrecord_id=" << tc.subrecord_id;
    EXPECT_DOUBLE_EQ(tc.expected_min, min_val);
    EXPECT_DOUBLE_EQ(tc.expected_max, max_val);
  }

  // Subrecords in [1..31] omitted from the switch safely trigger default:.
  for (unsigned int unmapped :
       {GSF_SWATH_BATHY_SUBRECORD_INTENSITY_SERIES_ARRAY,
        GSF_SWATH_BATHY_SUBRECORD_SONAR_VERT_UNCERT_ARRAY,
        GSF_SWATH_BATHY_SUBRECORD_SONAR_HORZ_UNCERT_ARRAY,
        GSF_SWATH_BATHY_SUBRECORD_DETECTION_WINDOW_ARRAY,
        GSF_SWATH_BATHY_SUBRECORD_MEAN_ABS_COEF_ARRAY}) {
    ping.scaleFactors.scaleTable[unmapped - 1].multiplier = 1.0;
    EXPECT_EQ(-1, gsfGetSwathBathyArrayMinMax(&ping, unmapped, &min_val,
                                              &max_val));
    EXPECT_EQ(GSF_UNRECOGNIZED_ARRAY_SUBRECORD_ID, gsfIntError());
  }
}

// --- Group E: gsfCopyRecords and gsfFree ---

TEST_F(GsfTest, CopyRecordsAndFreeDeepCopyAndRealloc) {
  // 1. Deep-copy all 30 beam arrays + brb_inten + SVP + parameters + comment +
  // history + nav errors + attitude into an empty target, then overwrite with
  // matching number_beams (avoiding upstream receive_heave/vertical_error/
  // brb_inten num_beams growth bugs in gsf.c:3060,3226,3449).
  gsfRecords target{};
  ZeroStructs(target);
  absl::Cleanup free_target = [&target] { gsfFree(&target); };

  auto populate_non_ping =
      [](gsfRecords& src, std::vector<double>& svp_d, std::vector<double>& svp_v,
         std::vector<std::string>& pparams, std::vector<std::string>& sparams,
         std::string& cmt, std::string& hcmd, std::string& hcmt,
         std::string& pos_type, std::vector<struct timespec>& att_t,
         std::vector<double>& att_p, std::vector<double>& att_r,
         std::vector<double>& att_h, std::vector<double>& att_hdg) {
        src.svp.number_points = static_cast<int>(svp_d.size());
        src.svp.depth = svp_d.data();
        src.svp.sound_speed = svp_v.data();

        src.process_parameters.number_parameters =
            static_cast<int>(pparams.size());
        for (size_t i = 0; i < pparams.size(); ++i) {
          src.process_parameters.param[i] = pparams[i].data();
          src.process_parameters.param_size[i] =
              static_cast<short>(pparams[i].size());
        }

        src.sensor_parameters.number_parameters =
            static_cast<int>(sparams.size());
        for (size_t i = 0; i < sparams.size(); ++i) {
          src.sensor_parameters.param[i] = sparams[i].data();
          src.sensor_parameters.param_size[i] =
              static_cast<short>(sparams[i].size());
        }

        src.comment.comment_length = static_cast<int>(cmt.size());
        src.comment.comment = cmt.empty() ? nullptr : cmt.data();

        CopyToFixedChars(src.history.host_name, "host-a");
        CopyToFixedChars(src.history.operator_name, "op-a");
        src.history.command_line = hcmd.data();
        src.history.comment = hcmt.data();

        src.hv_nav_error.position_type = pos_type.data();

        src.attitude.num_measurements = static_cast<short>(att_t.size());
        src.attitude.attitude_time = att_t.data();
        src.attitude.pitch = att_p.data();
        src.attitude.roll = att_r.data();
        src.attitude.heave = att_h.data();
        src.attitude.heading = att_hdg.data();
      };

  OwnedPingArrays p1(/*beams=*/4, /*samples_per_beam=*/2, /*include_all=*/true);
  gsfRecords src1{};
  ZeroStructs(src1);
  CopyToFixedChars(src1.header.version, GSF_VERSION);
  ASSERT_NO_FATAL_FAILURE(p1.Bind(src1.mb_ping));
  std::vector<double> svp_d1 = {1.0, 2.0};
  std::vector<double> svp_v1 = {1500.0, 1505.0};
  std::vector<std::string> pp1 = {"K0=V0", "K1=V1"};
  std::vector<std::string> sp1 = {"S0=A", "S1=B"};
  std::string cmt1 = "initial longer comment";
  std::string hcmd1 = "cmd1";
  std::string hcmt1 = "hist1";
  std::string pos1 = GSF_POS_TYPE_GPSU;
  std::vector<struct timespec> at1 = {{100, 0}, {101, 0}};
  std::vector<double> ap1 = {1.0, 2.0};
  std::vector<double> ar1 = {-1.0, -2.0};
  std::vector<double> ah1 = {0.1, 0.2};
  std::vector<double> ahdg1 = {10.0, 20.0};
  populate_non_ping(src1, svp_d1, svp_v1, pp1, sp1, cmt1, hcmd1, hcmt1, pos1,
                    at1, ap1, ar1, ah1, ahdg1);

  ASSERT_EQ(GSF_NORMAL, gsfCopyRecords(&target, &src1));
  EXPECT_NE(src1.mb_ping.depth, target.mb_ping.depth);
  EXPECT_DOUBLE_EQ(src1.mb_ping.depth[3], target.mb_ping.depth[3]);
  EXPECT_NE(src1.mb_ping.receive_heave, target.mb_ping.receive_heave);
  EXPECT_DOUBLE_EQ(src1.mb_ping.receive_heave[2],
                   target.mb_ping.receive_heave[2]);
  EXPECT_NE(src1.mb_ping.brb_inten, target.mb_ping.brb_inten);
  EXPECT_EQ(100u, target.mb_ping.brb_inten->time_series[0].samples[0]);
  EXPECT_NE(src1.svp.depth, target.svp.depth);
  EXPECT_STREQ("initial longer comment", target.comment.comment);
  EXPECT_STREQ(GSF_POS_TYPE_GPSU, target.hv_nav_error.position_type);
  EXPECT_EQ(2, target.attitude.num_measurements);

  // Overwrite populated target with larger brb_inten sample_count, SVP,
  // params, and attitude, keeping comment_length <= initial comment_length
  // (gsf.c:3620 quirk).
  OwnedPingArrays p2(/*beams=*/4, /*samples_per_beam=*/4, /*include_all=*/true);
  gsfRecords src2{};
  ZeroStructs(src2);
  ASSERT_NO_FATAL_FAILURE(p2.Bind(src2.mb_ping));
  std::vector<double> svp_d2 = {1.0, 2.0, 3.0, 4.0};
  std::vector<double> svp_v2 = {1500.0, 1502.0, 1504.0, 1506.0};
  std::vector<std::string> pp2 = {"K0=MUCH_LONGER_V0", "K1=LONGER_V1", "K2=V2"};
  std::vector<std::string> sp2 = {"S0=LONGER_VAL", "S1=LONGER_VAL2", "S2=Z"};
  std::string cmt2 = "shorter";
  std::string hcmd2 = "cmd2";
  std::string hcmt2 = "hist2";
  std::string pos2 = GSF_POS_TYPE_PPSD;
  std::vector<struct timespec> at2 = {{200, 0}, {201, 0}, {202, 0}, {203, 0}};
  std::vector<double> ap2 = {1.0, 2.0, 3.0, 4.0};
  std::vector<double> ar2 = {-1.0, -2.0, -3.0, -4.0};
  std::vector<double> ah2 = {0.1, 0.2, 0.3, 0.4};
  std::vector<double> ahdg2 = {10.0, 20.0, 30.0, 40.0};
  populate_non_ping(src2, svp_d2, svp_v2, pp2, sp2, cmt2, hcmd2, hcmt2, pos2,
                    at2, ap2, ar2, ah2, ahdg2);

  ASSERT_EQ(GSF_NORMAL, gsfCopyRecords(&target, &src2));
  EXPECT_EQ(4, target.svp.number_points);
  EXPECT_DOUBLE_EQ(1506.0, target.svp.sound_speed[3]);
  EXPECT_EQ(4, target.mb_ping.brb_inten->time_series[0].sample_count);
  EXPECT_EQ(4, target.attitude.num_measurements);
  EXPECT_STREQ("MUCH_LONGER_V0", target.process_parameters.param[0] + 3);

  // Overwrite with num_measurements == 0 to cover attitude free branch, then
  // restore 2 measurements before gsfFree runs.
  src2.attitude.num_measurements = 0;
  ASSERT_EQ(GSF_NORMAL, gsfCopyRecords(&target, &src2));
  EXPECT_EQ(nullptr, target.attitude.attitude_time);
  EXPECT_EQ(0, target.attitude.num_measurements);
  src2.attitude.num_measurements = 2;
  ASSERT_EQ(GSF_NORMAL, gsfCopyRecords(&target, &src2));
  EXPECT_EQ(2, target.attitude.num_measurements);

  // 2. Test mb_ping realloc across number_beams growth (2 -> 5) for the 28
  // safe arrays (excluding receive_heave, vertical_error, and brb_inten).
  gsfRecords grow_target{};
  ZeroStructs(grow_target);
  absl::Cleanup free_grow = [&grow_target] { gsfFree(&grow_target); };
  OwnedPingArrays pg1(/*beams=*/2, /*samples_per_beam=*/1,
                      /*include_all=*/false);
  gsfRecords gsrc1{};
  ZeroStructs(gsrc1);
  ASSERT_NO_FATAL_FAILURE(pg1.Bind(gsrc1.mb_ping));
  gsrc1.svp.number_points = 1;
  gsrc1.svp.depth = svp_d1.data();
  gsrc1.svp.sound_speed = svp_v1.data();
  ASSERT_EQ(GSF_NORMAL, gsfCopyRecords(&grow_target, &gsrc1));
  EXPECT_EQ(2, grow_target.mb_ping.number_beams);

  OwnedPingArrays pg2(/*beams=*/5, /*samples_per_beam=*/1,
                      /*include_all=*/false);
  gsfRecords gsrc2{};
  ZeroStructs(gsrc2);
  ASSERT_NO_FATAL_FAILURE(pg2.Bind(gsrc2.mb_ping));
  gsrc2.svp.number_points = 1;
  gsrc2.svp.depth = svp_d2.data();
  gsrc2.svp.sound_speed = svp_v2.data();
  ASSERT_EQ(GSF_NORMAL, gsfCopyRecords(&grow_target, &gsrc2));
  EXPECT_EQ(5, grow_target.mb_ping.number_beams);
  EXPECT_DOUBLE_EQ(pg2.depth[4], grow_target.mb_ping.depth[4]);
  EXPECT_DOUBLE_EQ(pg2.tvg_db[4], grow_target.mb_ping.TVG_dB[4]);

  // Verify explicit gsfFree zeroes pointers safely and handles double-call.
  gsfFree(&grow_target);
  EXPECT_EQ(nullptr, grow_target.mb_ping.depth);
  EXPECT_EQ(0, grow_target.mb_ping.number_beams);
  gsfFree(&grow_target);
}

// --- Group F: gsfInitializeMBParams, gsfPutMBParams, gsfGetMBParams ---

TEST_F(GsfTest, InitializeAndPutGetMBParamsRoundTripAndErrors) {
  gsfMBParams params{};
  ZeroStructs(params);
  gsfInitializeMBParams(&params);
  EXPECT_EQ(GSF_UNKNOWN_PARAM_INT, params.horizontal_datum);
  EXPECT_EQ(GSF_UNKNOWN_PARAM_INT, params.vertical_datum);
  EXPECT_EQ(GSF_PLATFORM_TYPE_SURFACE_SHIP, params.vessel_type);
  EXPECT_DOUBLE_EQ(GSF_UNKNOWN_PARAM_VALUE, params.to_apply.draft[0]);
  EXPECT_DOUBLE_EQ(GSF_UNKNOWN_PARAM_VALUE, params.applied.draft[1]);

  gsfRecords rec{};
  ZeroStructs(rec);
  EXPECT_EQ(-1, gsfPutMBParams(&params, &rec, kBadHandleZero, 2));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());
  EXPECT_EQ(-1, gsfPutMBParams(&params, &rec, kBadHandleOver, 2));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());

  // Empty process_parameters produces *numArrays == 0.
  gsfMBParams empty_decoded{};
  ZeroStructs(empty_decoded);
  int num_arrays = -1;
  ASSERT_EQ(GSF_NORMAL, gsfGetMBParams(&rec, &empty_decoded, &num_arrays));
  EXPECT_EQ(0, num_arrays);

  // Round-trip initialized ("UNKNWN") params through write + read on the same
  // handle so gsfFileTable[h-1].rec owns and cleans up param strings on reuse.
  const std::filesystem::path path = TestPath("mb_params_init.gsf");
  int h = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_CREATE, &h));
  absl::Cleanup close_h = [h] { gsfClose(h); };

  ASSERT_EQ(GSF_NORMAL, PutMBParamsSafely(&params, &rec, h, 2));
  gsfDataID id{};
  ZeroStructs(id);
  id.recordID = GSF_RECORD_PROCESSING_PARAMETERS;
  ASSERT_GT(gsfWrite(h, &id, &rec), 0);
  ASSERT_EQ(GSF_NORMAL, gsfSeek(h, GSF_REWIND));

  // Read back directly into rec so rec stays synced with ft->rec pointers.
  ASSERT_GT(gsfRead(h, GSF_RECORD_PROCESSING_PARAMETERS, &id, &rec, nullptr, 0),
            0);

  gsfMBParams decoded{};
  ZeroStructs(decoded);
  ASSERT_EQ(GSF_NORMAL, gsfGetMBParams(&rec, &decoded, &num_arrays));
  EXPECT_EQ(2, num_arrays);
  EXPECT_EQ(GSF_PLATFORM_TYPE_SURFACE_SHIP, decoded.vessel_type);
  EXPECT_EQ(GSF_H_DATUM_UND, decoded.horizontal_datum);
  EXPECT_EQ(GSF_V_DATUM_UNKNOWN, decoded.vertical_datum);
  EXPECT_EQ(GSF_DEPTH_CALC_UNKNOWN, decoded.depth_calculation);
  // atoi("UNKNWN") == 0 in gsf.c:7046; gsfPutMBParams uses numArrays (= 2) for
  // unknown transmitter/receiver counts (gsf.c:3930-3938, 4051).
  EXPECT_EQ(0, decoded.utc_offset);
  EXPECT_EQ(2, decoded.number_of_transmitters);
  EXPECT_EQ(2, decoded.number_of_receivers);
  EXPECT_DOUBLE_EQ(GSF_UNKNOWN_PARAM_VALUE, decoded.to_apply.draft[0]);
}

TEST_F(GsfTest, PutAndGetMBParamsPopulatedEnumsVDatumsAndFixedSizeErrors) {
  // 1. Round-trip populated numArrays == 1 (AUV / WGE / TRUE_DEPTHS / YES flags).
  {
    const std::filesystem::path p1 = TestPath("mb_params_num1.gsf");
    int h1 = 0;
    ASSERT_EQ(GSF_NORMAL, gsfOpen(p1.c_str(), GSF_CREATE, &h1));
    absl::Cleanup close_h1 = [h1] { gsfClose(h1); };

    gsfMBParams in{};
    ZeroStructs(in);
    gsfInitializeMBParams(&in);
    in.horizontal_datum = GSF_H_DATUM_WGE;
    in.vertical_datum = GSF_V_DATUM_MLLW;
    in.utc_offset = -5;
    in.number_of_transmitters = 1;
    in.number_of_receivers = 1;
    in.roll_reference = GSF_HORIZONTAL_PITCH_AXIS;
    in.roll_compensated = GSF_COMPENSATED;
    in.pitch_compensated = GSF_COMPENSATED;
    in.heave_compensated = GSF_COMPENSATED;
    in.tide_compensated = GSF_COMPENSATED;
    in.ray_tracing = GSF_COMPENSATED;
    in.depth_calculation = GSF_TRUE_DEPTHS;
    in.vessel_type = GSF_PLATFORM_TYPE_AUV;
    in.full_raw_data = GSF_TRUE;
    in.msb_applied_to_attitude = GSF_TRUE;
    in.heave_removed_from_gps_tc = GSF_TRUE;
    FillMBOffsets(in.to_apply, 1.0);
    FillMBOffsets(in.applied, -2.0);

    gsfRecords rec{};
    gsfDataID id{};
    ZeroStructs(rec, id);
    ASSERT_EQ(GSF_NORMAL, PutMBParamsSafely(&in, &rec, h1, 1));
    id.recordID = GSF_RECORD_PROCESSING_PARAMETERS;
    ASSERT_GT(gsfWrite(h1, &id, &rec), 0);
    ASSERT_EQ(GSF_NORMAL, gsfSeek(h1, GSF_REWIND));
    ASSERT_GT(
        gsfRead(h1, GSF_RECORD_PROCESSING_PARAMETERS, &id, &rec, nullptr, 0),
        0);

    gsfMBParams out{};
    ZeroStructs(out);
    int num_arrays = 0;
    ASSERT_EQ(GSF_NORMAL, gsfGetMBParams(&rec, &out, &num_arrays));
    EXPECT_EQ(1, num_arrays);
    EXPECT_THAT(out.start_of_epoch, HasSubstr("1970/001"));
    EXPECT_EQ(GSF_H_DATUM_WGE, out.horizontal_datum);
    EXPECT_EQ(GSF_V_DATUM_MLLW, out.vertical_datum);
    EXPECT_EQ(-5, out.utc_offset);
    EXPECT_EQ(1, out.number_of_transmitters);
    EXPECT_EQ(1, out.number_of_receivers);
    EXPECT_EQ(GSF_HORIZONTAL_PITCH_AXIS, out.roll_reference);
    EXPECT_EQ(GSF_COMPENSATED, out.roll_compensated);
    EXPECT_EQ(GSF_COMPENSATED, out.pitch_compensated);
    EXPECT_EQ(GSF_COMPENSATED, out.heave_compensated);
    EXPECT_EQ(GSF_COMPENSATED, out.tide_compensated);
    EXPECT_EQ(GSF_COMPENSATED, out.ray_tracing);
    EXPECT_EQ(GSF_TRUE_DEPTHS, out.depth_calculation);
    EXPECT_EQ(GSF_PLATFORM_TYPE_AUV, out.vessel_type);
    EXPECT_EQ(GSF_TRUE, out.full_raw_data);
    EXPECT_EQ(GSF_TRUE, out.msb_applied_to_attitude);
    EXPECT_EQ(GSF_TRUE, out.heave_removed_from_gps_tc);
    ExpectOffsetsEqual(in.to_apply, out.to_apply, 1);
    ExpectOffsetsEqual(in.applied, out.applied, 1);
  }

  // 2. Round-trip populated numArrays == 2 (ROTV / NAR / NO flags) + parameter
  // buffer growth on an existing handle via PutMBParamsSafely.
  {
    const std::filesystem::path p2 = TestPath("mb_params_num2.gsf");
    int h2 = 0;
    ASSERT_EQ(GSF_NORMAL, gsfOpen(p2.c_str(), GSF_CREATE, &h2));
    absl::Cleanup close_h2 = [h2] { gsfClose(h2); };

    gsfMBParams short_p{};
    gsfRecords rec{};
    gsfMBParams in{};
    ZeroStructs(short_p, rec, in);
    gsfInitializeMBParams(&short_p);
    ASSERT_EQ(GSF_NORMAL, PutMBParamsSafely(&short_p, &rec, h2, 1));

    gsfInitializeMBParams(&in);
    in.horizontal_datum = GSF_H_DATUM_NAR;
    in.vertical_datum = GSF_V_DATUM_MSL;
    in.utc_offset = 12;
    in.number_of_transmitters = 2;
    in.number_of_receivers = 2;
    in.roll_reference = GSF_ROTATED_PITCH_AXIS;
    in.roll_compensated = GSF_UNCOMPENSATED;
    in.pitch_compensated = GSF_UNCOMPENSATED;
    in.heave_compensated = GSF_UNCOMPENSATED;
    in.tide_compensated = GSF_UNCOMPENSATED;
    in.ray_tracing = GSF_UNCOMPENSATED;
    in.depth_calculation = GSF_DEPTHS_RE_1500_MS;
    in.vessel_type = GSF_PLATFORM_TYPE_ROTV;
    in.full_raw_data = GSF_FALSE;
    in.msb_applied_to_attitude = GSF_FALSE;
    in.heave_removed_from_gps_tc = GSF_FALSE;
    FillMBOffsets(in.to_apply, 3.0);
    FillMBOffsets(in.applied, -4.0);

    // Grows shorter strings into 2-array strings -> hits gsfSetParam realloc.
    ASSERT_EQ(GSF_NORMAL, PutMBParamsSafely(&in, &rec, h2, 2));
    gsfDataID id{};
    ZeroStructs(id);
    id.recordID = GSF_RECORD_PROCESSING_PARAMETERS;
    ASSERT_GT(gsfWrite(h2, &id, &rec), 0);
    ASSERT_EQ(GSF_NORMAL, gsfSeek(h2, GSF_REWIND));
    ASSERT_GT(
        gsfRead(h2, GSF_RECORD_PROCESSING_PARAMETERS, &id, &rec, nullptr, 0),
        0);

    gsfMBParams out{};
    ZeroStructs(out);
    int num_arrays = 0;
    ASSERT_EQ(GSF_NORMAL, gsfGetMBParams(&rec, &out, &num_arrays));
    EXPECT_EQ(2, num_arrays);
    EXPECT_EQ(GSF_H_DATUM_NAR, out.horizontal_datum);
    EXPECT_EQ(GSF_V_DATUM_MSL, out.vertical_datum);
    EXPECT_EQ(12, out.utc_offset);
    EXPECT_EQ(2, out.number_of_transmitters);
    EXPECT_EQ(2, out.number_of_receivers);
    EXPECT_EQ(GSF_ROTATED_PITCH_AXIS, out.roll_reference);
    EXPECT_EQ(GSF_PLATFORM_TYPE_ROTV, out.vessel_type);
    // gsfPutMBParams emits RELATIVE_TO_1500_MS while gsfGetMBParams checks for
    // CALCULATED_RE_1500_MS (gsf.c:4076 vs 6994).
    EXPECT_EQ(GSF_DEPTH_CALC_UNKNOWN, out.depth_calculation);
    ExpectOffsetsEqual(in.to_apply, out.to_apply, 2);
    ExpectOffsetsEqual(in.applied, out.applied, 2);
  }

  // 3. Direct gsfGetMBParams parsing of CALCULATED_RE_1500_MS and out-of-range
  // integer tags.
  {
    std::array<std::string, 4> kvs = {
        "DEPTH_CALCULATION=CALCULATED_RE_1500_MS",
        "UTC_OFFSET=99",
        "NUMBER_OF_TRANSMITTERS=99",
        "NUMBER_OF_RECEIVERS=99",
    };
    gsfRecords rec{};
    gsfMBParams out{};
    ZeroStructs(rec, out);
    rec.process_parameters.number_parameters = static_cast<int>(kvs.size());
    for (size_t i = 0; i < kvs.size(); ++i) {
      rec.process_parameters.param[i] = kvs[i].data();
      rec.process_parameters.param_size[i] = static_cast<short>(kvs[i].size());
    }
    int num_arrays = 0;
    ASSERT_EQ(GSF_NORMAL, gsfGetMBParams(&rec, &out, &num_arrays));
    EXPECT_EQ(GSF_DEPTHS_RE_1500_MS, out.depth_calculation);
    EXPECT_EQ(GSF_UNKNOWN_PARAM_INT, out.utc_offset);
    EXPECT_EQ(0, out.number_of_transmitters);
    EXPECT_EQ(0, out.number_of_receivers);
  }

  // 4. Partially unknown [0] / [1] array values in numArrays == 2.
  {
    const std::filesystem::path pm = TestPath("mb_params_partial.gsf");
    int hm = 0;
    ASSERT_EQ(GSF_NORMAL, gsfOpen(pm.c_str(), GSF_CREATE, &hm));
    absl::Cleanup close_hm = [hm] { gsfClose(hm); };

    gsfMBParams partial{};
    ZeroStructs(partial);
    gsfInitializeMBParams(&partial);
    FillMBOffsets(partial.to_apply, 2.0);
    FillMBOffsets(partial.applied, 3.0);
    partial.to_apply.draft[0] = GSF_UNKNOWN_PARAM_VALUE;
    partial.applied.draft[1] = GSF_UNKNOWN_PARAM_VALUE;
    partial.to_apply.rx_transducer_x_offset[1] = GSF_UNKNOWN_PARAM_VALUE;
    partial.applied.rx_transducer_heading_offset[0] = GSF_UNKNOWN_PARAM_VALUE;

    gsfRecords rec{};
    gsfDataID id{};
    gsfMBParams out{};
    ZeroStructs(rec, id, out);
    ASSERT_EQ(GSF_NORMAL, PutMBParamsSafely(&partial, &rec, hm, 2));
    id.recordID = GSF_RECORD_PROCESSING_PARAMETERS;
    ASSERT_GT(gsfWrite(hm, &id, &rec), 0);
    ASSERT_EQ(GSF_NORMAL, gsfSeek(hm, GSF_REWIND));
    ASSERT_GT(
        gsfRead(hm, GSF_RECORD_PROCESSING_PARAMETERS, &id, &rec, nullptr, 0),
        0);
    int num_arrays = 0;
    ASSERT_EQ(GSF_NORMAL, gsfGetMBParams(&rec, &out, &num_arrays));
    EXPECT_DOUBLE_EQ(GSF_UNKNOWN_PARAM_VALUE, out.to_apply.draft[0]);
    EXPECT_DOUBLE_EQ(GSF_UNKNOWN_PARAM_VALUE, out.applied.draft[0]);
  }

  // 5. All 23 vertical_datum enum values round-trip cleanly; start with
  // GSF_V_DATUM_UNKNOWN ("TIDAL_DATUM=UNKNOWN", 19 chars) so param_size is max.
  {
    const std::filesystem::path pv = TestPath("mb_params_vdatums.gsf");
    int hv = 0;
    ASSERT_EQ(GSF_NORMAL, gsfOpen(pv.c_str(), GSF_CREATE, &hv));
    absl::Cleanup close_hv = [hv] { gsfClose(hv); };

    gsfMBParams vp{};
    gsfRecords rec{};
    gsfDataID id{};
    ZeroStructs(vp, rec, id);
    gsfInitializeMBParams(&vp);
    vp.vertical_datum = GSF_V_DATUM_UNKNOWN;
    ASSERT_EQ(GSF_NORMAL, PutMBParamsSafely(&vp, &rec, hv, 1));
    id.recordID = GSF_RECORD_PROCESSING_PARAMETERS;
    ASSERT_GT(gsfWrite(hv, &id, &rec), 0);
    ASSERT_EQ(GSF_NORMAL, gsfSeek(hv, GSF_REWIND));
    // Read back into rec directly so rec does not hold pointers freed by
    // gsfDecodeProcessingParameters before subsequent PutMBParamsSafely calls.
    ASSERT_GT(
        gsfRead(hv, GSF_RECORD_PROCESSING_PARAMETERS, &id, &rec, nullptr, 0),
        0);

    for (int vdatum = GSF_V_DATUM_UNKNOWN; vdatum <= GSF_V_DATUM_MAX_INDEX;
         ++vdatum) {
      vp.vertical_datum = vdatum;
      ASSERT_EQ(GSF_NORMAL, PutMBParamsSafely(&vp, &rec, hv, 1));
      gsfMBParams out{};
      ZeroStructs(out);
      int num_arrays = 0;
      ASSERT_EQ(GSF_NORMAL, gsfGetMBParams(&rec, &out, &num_arrays));
      EXPECT_EQ(vdatum, out.vertical_datum) << "vdatum=" << vdatum;
    }
  }

  // 6. GSF_PARAM_SIZE_FIXED errors in GSF_UPDATE and GSF_UPDATE_INDEX modes.
  {
    const std::filesystem::path p_small = TestPath("fixed_small.gsf");
    int hs = 0;
    ASSERT_EQ(GSF_NORMAL, gsfOpen(p_small.c_str(), GSF_CREATE, &hs));
    {
      absl::Cleanup close_hs = [hs] { gsfClose(hs); };
      std::string tiny = "A=1";
      gsfDataID id{};
      gsfRecords rec{};
      ZeroStructs(id, rec);
      id.recordID = GSF_RECORD_PROCESSING_PARAMETERS;
      rec.process_parameters.number_parameters = 1;
      rec.process_parameters.param[0] = tiny.data();
      rec.process_parameters.param_size[0] = static_cast<short>(tiny.size());
      ASSERT_GT(gsfWrite(hs, &id, &rec), 0);
    }

    ASSERT_EQ(GSF_NORMAL, gsfOpen(p_small.c_str(), GSF_UPDATE, &hs));
    {
      absl::Cleanup close_hs = [hs] { gsfClose(hs); };
      gsfDataID id{};
      gsfRecords read_rec{};
      gsfMBParams p{};
      ZeroStructs(id, read_rec, p);
      ASSERT_GT(gsfRead(hs, GSF_RECORD_PROCESSING_PARAMETERS, &id, &read_rec,
                        nullptr, 0),
                0);
      gsfInitializeMBParams(&p);
      EXPECT_EQ(-1, PutMBParamsSafely(&p, &read_rec, hs, 1));
      EXPECT_EQ(GSF_PARAM_SIZE_FIXED, gsfIntError());
    }

    // Reopen existing initialized record in GSF_UPDATE_INDEX and try growing
    // ROLL_REFERENCE ("UNKNWN" -> "HORIZONTAL_PITCH_AXIS") inside gsfSetParam.
    const std::filesystem::path p_grow = TestPath("fixed_grow.gsf");
    int hg = 0;
    ASSERT_EQ(GSF_NORMAL, gsfOpen(p_grow.c_str(), GSF_CREATE, &hg));
    {
      absl::Cleanup close_hg = [hg] { gsfClose(hg); };
      gsfMBParams init{};
      gsfRecords rec{};
      gsfDataID id{};
      ZeroStructs(init, rec, id);
      gsfInitializeMBParams(&init);
      ASSERT_EQ(GSF_NORMAL, PutMBParamsSafely(&init, &rec, hg, 2));
      id.recordID = GSF_RECORD_PROCESSING_PARAMETERS;
      ASSERT_GT(gsfWrite(hg, &id, &rec), 0);
      ASSERT_EQ(GSF_NORMAL, gsfSeek(hg, GSF_REWIND));
      ASSERT_GT(
          gsfRead(hg, GSF_RECORD_PROCESSING_PARAMETERS, &id, &rec, nullptr, 0),
          0);
    }

    ASSERT_EQ(GSF_NORMAL, gsfOpen(p_grow.c_str(), GSF_UPDATE_INDEX, &hg));
    absl::Cleanup close_hg = [hg] { gsfClose(hg); };
    gsfDataID id{};
    gsfRecords rrec{};
    gsfMBParams grown{};
    ZeroStructs(id, rrec, grown);
    id.record_number = 1;
    ASSERT_GT(gsfRead(hg, GSF_RECORD_PROCESSING_PARAMETERS, &id, &rrec, nullptr,
                      0),
              0);
    gsfInitializeMBParams(&grown);
    grown.roll_reference = GSF_HORIZONTAL_PITCH_AXIS;
    EXPECT_EQ(-1, PutMBParamsSafely(&grown, &rrec, hg, 2));
    EXPECT_EQ(GSF_PARAM_SIZE_FIXED, gsfIntError());
  }
}

TEST_F(GsfTest, PutMBParamsRejectsOutOfRangeOffsets) {
  const std::filesystem::path path = TestPath("mb_params_oob.gsf");
  int h = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_CREATE, &h));
  absl::Cleanup close_h = [h] { gsfClose(h); };

  // Seed handle with valid numArrays == 2 record and read it back so
  // gsfFileTable[h-1].rec.process_parameters.number_parameters == 63.
  gsfMBParams seed{};
  gsfRecords rec{};
  gsfDataID id{};
  ZeroStructs(seed, rec, id);
  gsfInitializeMBParams(&seed);
  seed.number_of_transmitters = 2;
  seed.number_of_receivers = 2;
  seed.roll_reference = GSF_HORIZONTAL_PITCH_AXIS;
  seed.depth_calculation = GSF_DEPTHS_RE_1500_MS;
  FillMBOffsets(seed.to_apply, 1.0);
  FillMBOffsets(seed.applied, -1.0);

  ASSERT_EQ(GSF_NORMAL, PutMBParamsSafely(&seed, &rec, h, 2));
  id.recordID = GSF_RECORD_PROCESSING_PARAMETERS;
  ASSERT_GT(gsfWrite(h, &id, &rec), 0);
  ASSERT_EQ(GSF_NORMAL, gsfSeek(h, GSF_REWIND));
  // Read back into rec directly so rec keeps ft->rec's updated pointers across
  // subsequent PutMBParamsSafely calls.
  ASSERT_GT(
      gsfRead(h, GSF_RECORD_PROCESSING_PARAMETERS, &id, &rec, nullptr, 0), 0);

  using ScalarPtr = double gsfMBOffsets::*;
  constexpr auto kScalarFields = std::to_array<ScalarPtr>({
      &gsfMBOffsets::position_x_offset,
      &gsfMBOffsets::position_y_offset,
      &gsfMBOffsets::position_z_offset,
      &gsfMBOffsets::antenna_x_offset,
      &gsfMBOffsets::antenna_y_offset,
      &gsfMBOffsets::antenna_z_offset,
      &gsfMBOffsets::mru_pitch_bias,
      &gsfMBOffsets::mru_roll_bias,
      &gsfMBOffsets::mru_heading_bias,
      &gsfMBOffsets::mru_x_offset,
      &gsfMBOffsets::mru_y_offset,
      &gsfMBOffsets::mru_z_offset,
      &gsfMBOffsets::center_of_rotation_x_offset,
      &gsfMBOffsets::center_of_rotation_y_offset,
      &gsfMBOffsets::center_of_rotation_z_offset,
      &gsfMBOffsets::position_latency,
      &gsfMBOffsets::attitude_latency,
      &gsfMBOffsets::depth_sensor_latency,
      &gsfMBOffsets::depth_sensor_x_offset,
      &gsfMBOffsets::depth_sensor_y_offset,
      &gsfMBOffsets::depth_sensor_z_offset,
  });

  for (ScalarPtr field : kScalarFields) {
    gsfMBParams bad{};
    ZeroStructs(bad);
    bad = seed;
    bad.to_apply.*field = 1.0e6;
    EXPECT_EQ(-1, PutMBParamsSafely(&bad, &rec, h, 2));
    EXPECT_EQ(GSF_PROCESS_PARAM_RECORD_ENCODE_FAILED, gsfIntError());

    ZeroStructs(bad);
    bad = seed;
    bad.applied.*field = -1.0e6;
    EXPECT_EQ(-1, PutMBParamsSafely(&bad, &rec, h, 2));
    EXPECT_EQ(GSF_PROCESS_PARAM_RECORD_ENCODE_FAILED, gsfIntError());
  }

  using ArrayPtr = double (gsfMBOffsets::*)[GSF_MAX_OFFSETS];
  constexpr auto kArrayFields = std::to_array<ArrayPtr>({
      &gsfMBOffsets::draft,
      &gsfMBOffsets::pitch_bias,
      &gsfMBOffsets::roll_bias,
      &gsfMBOffsets::gyro_bias,
      &gsfMBOffsets::transducer_x_offset,
      &gsfMBOffsets::transducer_y_offset,
      &gsfMBOffsets::transducer_z_offset,
      &gsfMBOffsets::transducer_pitch_offset,
      &gsfMBOffsets::transducer_roll_offset,
      &gsfMBOffsets::transducer_heading_offset,
      &gsfMBOffsets::rx_transducer_x_offset,
      &gsfMBOffsets::rx_transducer_y_offset,
      &gsfMBOffsets::rx_transducer_z_offset,
      &gsfMBOffsets::rx_transducer_pitch_offset,
      &gsfMBOffsets::rx_transducer_roll_offset,
      &gsfMBOffsets::rx_transducer_heading_offset,
  });

  for (ArrayPtr field : kArrayFields) {
    // numArrays == 1 checks array[0] single-head branch.
    {
      gsfMBParams bad{};
      ZeroStructs(bad);
      bad = seed;
      bad.number_of_transmitters = 1;
      bad.number_of_receivers = 1;
      (bad.to_apply.*field)[0] = 1.0e6;
      EXPECT_EQ(-1, PutMBParamsSafely(&bad, &rec, h, 1));
      EXPECT_EQ(GSF_PROCESS_PARAM_RECORD_ENCODE_FAILED, gsfIntError());

      ZeroStructs(bad);
      bad = seed;
      bad.number_of_transmitters = 1;
      bad.number_of_receivers = 1;
      (bad.applied.*field)[0] = -1.0e6;
      EXPECT_EQ(-1, PutMBParamsSafely(&bad, &rec, h, 1));
      EXPECT_EQ(GSF_PROCESS_PARAM_RECORD_ENCODE_FAILED, gsfIntError());
    }
    // numArrays == 2 checks array[0] and array[1] dual-head branches.
    for (int idx = 0; idx < GSF_MAX_OFFSETS; ++idx) {
      gsfMBParams bad{};
      ZeroStructs(bad);
      bad = seed;
      (bad.to_apply.*field)[idx] = 1.0e6;
      EXPECT_EQ(-1, PutMBParamsSafely(&bad, &rec, h, 2));
      EXPECT_EQ(GSF_PROCESS_PARAM_RECORD_ENCODE_FAILED, gsfIntError());

      ZeroStructs(bad);
      bad = seed;
      (bad.applied.*field)[idx] = -1.0e6;
      EXPECT_EQ(-1, PutMBParamsSafely(&bad, &rec, h, 2));
      EXPECT_EQ(GSF_PROCESS_PARAM_RECORD_ENCODE_FAILED, gsfIntError());
    }
  }
}

// --- Group G: Sensor Helpers and Error String Mapping ---

TEST_F(GsfTest, GetSwathBathyBeamWidthsCoversAllSensors) {
  struct WidthCase {
    int sensor_id;
    int sub_mode;
    double custom_fa;
    double custom_ath;
    int expected_status;
    double expected_fa;
    double expected_ath;
  };
  constexpr auto kCases = std::to_array<WidthCase>({
      {GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC, 0, 0, 0, GSF_NORMAL,
       2.666666666667, 2.666666666667},
      {GSF_SWATH_BATHY_SUBRECORD_EM12_SPECIFIC, 0, 0, 0, GSF_NORMAL, 1.7, 4.4},
      {GSF_SWATH_BATHY_SUBRECORD_EM100_SPECIFIC, 1, 0, 0, GSF_NORMAL, 3.0, 2.5},
      {GSF_SWATH_BATHY_SUBRECORD_EM100_SPECIFIC, 2, 0, 0, GSF_NORMAL, 3.0, 5.5},
      {GSF_SWATH_BATHY_SUBRECORD_EM100_SPECIFIC, 3, 0, 0, GSF_NORMAL, 3.0, 2.0},
      {GSF_SWATH_BATHY_SUBRECORD_EM100_SPECIFIC, 99, 0, 0, -1, 3.0,
       GSF_BEAM_WIDTH_UNKNOWN},
      {GSF_SWATH_BATHY_SUBRECORD_EM950_SPECIFIC, 0, 0, 0, GSF_NORMAL, 3.3, 3.3},
      {GSF_SWATH_BATHY_SUBRECORD_EM1000_SPECIFIC, 0, 0, 0, GSF_NORMAL, 3.3,
       3.3},
      // t_gsfEM121ASpecific.beam_width is an int field (gsf.h:460).
      {GSF_SWATH_BATHY_SUBRECORD_EM121A_SPECIFIC, 0, 1.0, 1.0, GSF_NORMAL, 1.0,
       1.0},
      {GSF_SWATH_BATHY_SUBRECORD_EM121_SPECIFIC, 0, 2.0, 2.0, GSF_NORMAL, 2.0,
       2.0},
      {GSF_SWATH_BATHY_SUBRECORD_SASS_SPECIFIC, 0, 0, 0, -1,
       GSF_BEAM_WIDTH_UNKNOWN, GSF_BEAM_WIDTH_UNKNOWN},
      {GSF_SWATH_BATHY_SUBRECORD_SEAMAP_SPECIFIC, 0, 0, 0, -1,
       GSF_BEAM_WIDTH_UNKNOWN, GSF_BEAM_WIDTH_UNKNOWN},
      {GSF_SWATH_BATHY_SUBRECORD_SEABAT_SPECIFIC,
       GSF_SEABAT_WIDE_MODE | GSF_SEABAT_9003, 0, 0, GSF_NORMAL, 10.0, 3.0},
      {GSF_SWATH_BATHY_SUBRECORD_SEABAT_SPECIFIC, 0, 0, 0, -1, 1.5, 1.5},
      {GSF_SWATH_BATHY_SUBRECORD_SEABAT_II_SPECIFIC, 0, 1.8, 1.9, GSF_NORMAL,
       1.8, 1.9},
      {GSF_SWATH_BATHY_SUBRECORD_SEABAT_8101_SPECIFIC, 0, 1.4, 1.6, GSF_NORMAL,
       1.4, 1.6},
      {GSF_SWATH_BATHY_SUBRECORD_SEABEAM_2112_SPECIFIC, 0, 0, 0, GSF_NORMAL,
       2.0, 2.0},
      {GSF_SWATH_BATHY_SUBRECORD_ELAC_MKII_SPECIFIC, 0, 0, 0, GSF_NORMAL, 2.0,
       2.0},
      {GSF_SWATH_BATHY_SUBRECORD_CMP_SASS_SPECIFIC, 0, 0, 0, GSF_NORMAL, 1.0,
       1.0},
      {GSF_SWATH_BATHY_SUBRECORD_EM300_SPECIFIC, 0, 0.0, 0.0, GSF_NORMAL, 1.5,
       1.5},
      {GSF_SWATH_BATHY_SUBRECORD_EM1002_SPECIFIC, 0, 2.2, 2.4, GSF_NORMAL, 2.2,
       2.4},
      {GSF_SWATH_BATHY_SUBRECORD_EM300_RAW_SPECIFIC, 0, 0.0, 0.0, GSF_NORMAL,
       1.5, 1.5},
      {GSF_SWATH_BATHY_SUBRECORD_EM2000_RAW_SPECIFIC, 0, 0.8, 0.9, GSF_NORMAL,
       0.8, 0.9},
      {GSF_SWATH_BATHY_SUBRECORD_EM122_SPECIFIC, 0, 0.0, 0.0, GSF_NORMAL, 1.0,
       1.0},
      {GSF_SWATH_BATHY_SUBRECORD_EM710_SPECIFIC, 0, 0.5, 0.6, GSF_NORMAL, 0.5,
       0.6},
      {GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC, 0, 1.5, 1.5, GSF_NORMAL,
       1.5, 1.5},
      {GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC, 0, 1.1, 0.5, GSF_NORMAL,
       1.1, 0.5},
      {GSF_SWATH_BATHY_SUBRECORD_RESON_TSERIES_SPECIFIC, 0, 0.7, 0.8,
       GSF_NORMAL, 0.7, 0.8},
      {GSF_SWATH_BATHY_SUBRECORD_GEOSWATH_PLUS_SPECIFIC, 100, 0, 0, GSF_NORMAL,
       0.9, GSF_BEAM_WIDTH_UNKNOWN},
      {GSF_SWATH_BATHY_SUBRECORD_GEOSWATH_PLUS_SPECIFIC, 250, 0, 0, GSF_NORMAL,
       0.5, GSF_BEAM_WIDTH_UNKNOWN},
      {GSF_SWATH_BATHY_SUBRECORD_GEOSWATH_PLUS_SPECIFIC, 500, 0, 0, GSF_NORMAL,
       0.5, GSF_BEAM_WIDTH_UNKNOWN},
      {GSF_SWATH_BATHY_SUBRECORD_GEOSWATH_PLUS_SPECIFIC, 999, 0, 0, GSF_NORMAL,
       GSF_BEAM_WIDTH_UNKNOWN, GSF_BEAM_WIDTH_UNKNOWN},
      {GSF_SWATH_BATHY_SUBRECORD_KLEIN_5410_BSS_SPECIFIC, 0, 0, 0, GSF_NORMAL,
       GSF_BEAM_WIDTH_UNKNOWN, GSF_BEAM_WIDTH_UNKNOWN},
      {GSF_SWATH_BATHY_SUBRECORD_DELTA_T_SPECIFIC, 0, 0.0, 0.0, GSF_NORMAL, 3.0,
       3.0},
      {GSF_SWATH_BATHY_SUBRECORD_DELTA_T_SPECIFIC, 0, 2.5, 2.7, GSF_NORMAL, 2.5,
       2.7},
      {GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2024_SPECIFIC, 0, 0.5, 0.5, GSF_NORMAL,
       0.5, 0.5},
      {GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC, 0, 0.4, 0.6, GSF_NORMAL, 0.4,
       0.6},
      {999, 0, 0, 0, -1, GSF_BEAM_WIDTH_UNKNOWN, GSF_BEAM_WIDTH_UNKNOWN},
  });

  for (const WidthCase& tc : kCases) {
    gsfRecords data{};
    ZeroStructs(data);
    data.mb_ping.sensor_id = tc.sensor_id;
    switch (tc.sensor_id) {
      case GSF_SWATH_BATHY_SUBRECORD_EM100_SPECIFIC:
        data.mb_ping.sensor_data.gsfEM100Specific.mode = tc.sub_mode;
        break;
      case GSF_SWATH_BATHY_SUBRECORD_EM121A_SPECIFIC:
        data.mb_ping.sensor_data.gsfEM121ASpecific.beam_width = tc.custom_fa;
        break;
      case GSF_SWATH_BATHY_SUBRECORD_EM121_SPECIFIC:
        data.mb_ping.sensor_data.gsfEM121Specific.beam_width = tc.custom_fa;
        break;
      case GSF_SWATH_BATHY_SUBRECORD_SEABAT_SPECIFIC:
        data.mb_ping.sensor_data.gsfSeaBatSpecific.mode = tc.sub_mode;
        break;
      case GSF_SWATH_BATHY_SUBRECORD_SEABAT_II_SPECIFIC:
        data.mb_ping.sensor_data.gsfSeaBatIISpecific.fore_aft_bw = tc.custom_fa;
        data.mb_ping.sensor_data.gsfSeaBatIISpecific.athwart_bw =
            tc.custom_ath;
        break;
      case GSF_SWATH_BATHY_SUBRECORD_SEABAT_8101_SPECIFIC:
        data.mb_ping.sensor_data.gsfSeaBat8101Specific.fore_aft_bw =
            tc.custom_fa;
        data.mb_ping.sensor_data.gsfSeaBat8101Specific.athwart_bw =
            tc.custom_ath;
        break;
      case GSF_SWATH_BATHY_SUBRECORD_EM300_SPECIFIC:
      case GSF_SWATH_BATHY_SUBRECORD_EM1002_SPECIFIC:
        data.mb_ping.sensor_data.gsfEM3Specific.run_time[0]
            .transmit_beam_width = tc.custom_fa;
        data.mb_ping.sensor_data.gsfEM3Specific.run_time[0].receive_beam_width =
            tc.custom_ath;
        break;
      case GSF_SWATH_BATHY_SUBRECORD_EM300_RAW_SPECIFIC:
      case GSF_SWATH_BATHY_SUBRECORD_EM2000_RAW_SPECIFIC:
        data.mb_ping.sensor_data.gsfEM3RawSpecific.run_time.tx_beam_width =
            tc.custom_fa;
        data.mb_ping.sensor_data.gsfEM3RawSpecific.run_time.rx_beam_width =
            tc.custom_ath;
        break;
      case GSF_SWATH_BATHY_SUBRECORD_EM122_SPECIFIC:
      case GSF_SWATH_BATHY_SUBRECORD_EM710_SPECIFIC:
        data.mb_ping.sensor_data.gsfEM4Specific.run_time.tx_beam_width =
            tc.custom_fa;
        data.mb_ping.sensor_data.gsfEM4Specific.run_time.rx_beam_width =
            tc.custom_ath;
        break;
      case GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC:
        data.mb_ping.sensor_data.gsfReson8100Specific.fore_aft_bw =
            tc.custom_fa;
        data.mb_ping.sensor_data.gsfReson8100Specific.athwart_bw =
            tc.custom_ath;
        break;
      case GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC:
        data.mb_ping.sensor_data.gsfReson7100Specific.projector_beam_wdth_vert =
            tc.custom_fa;
        data.mb_ping.sensor_data.gsfReson7100Specific.receive_beam_width =
            tc.custom_ath;
        break;
      case GSF_SWATH_BATHY_SUBRECORD_RESON_TSERIES_SPECIFIC:
        data.mb_ping.sensor_data.gsfResonTSeriesSpecific
            .projector_beam_wdth_vert = tc.custom_fa;
        data.mb_ping.sensor_data.gsfResonTSeriesSpecific.receive_beam_width =
            tc.custom_ath;
        break;
      case GSF_SWATH_BATHY_SUBRECORD_GEOSWATH_PLUS_SPECIFIC:
        data.mb_ping.sensor_data.gsfGeoSwathPlusSpecific.model_number =
            tc.sub_mode;
        break;
      case GSF_SWATH_BATHY_SUBRECORD_DELTA_T_SPECIFIC:
        data.mb_ping.sensor_data.gsfDeltaTSpecific.fore_aft_beamwidth =
            tc.custom_fa;
        data.mb_ping.sensor_data.gsfDeltaTSpecific.athwartships_beamwidth =
            tc.custom_ath;
        break;
      case GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2024_SPECIFIC:
        data.mb_ping.sensor_data.gsfR2SonicSpecific.tx_beamwidth_vert =
            tc.custom_fa;
        data.mb_ping.sensor_data.gsfR2SonicSpecific.tx_beamwidth_horiz =
            tc.custom_ath;
        break;
      case GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC:
        data.mb_ping.sensor_data.gsfKMALLSpecific.transmitArraySizeUsed_deg =
            tc.custom_fa;
        data.mb_ping.sensor_data.gsfKMALLSpecific.receiveArraySizeUsed_deg =
            tc.custom_ath;
        break;
      default:
        break;
    }

    double fa = -999.0;
    double ath = -999.0;
    EXPECT_EQ(tc.expected_status, gsfGetSwathBathyBeamWidths(&data, &fa, &ath))
        << "sensor_id=" << tc.sensor_id;
    EXPECT_DOUBLE_EQ(tc.expected_fa, fa) << "sensor_id=" << tc.sensor_id;
    EXPECT_DOUBLE_EQ(tc.expected_ath, ath) << "sensor_id=" << tc.sensor_id;
    if (tc.sensor_id == 999) {
      EXPECT_EQ(GSF_UNRECOGNIZED_SENSOR_ID, gsfIntError());
    }
  }
}

TEST_F(GsfTest, IsStarboardPingCoversDualHeadSensors) {
  {
    gsfRecords d{};
    ZeroStructs(d);
    d.mb_ping.sensor_id = GSF_SWATH_BATHY_SUBRECORD_GEOSWATH_PLUS_SPECIFIC;
    d.mb_ping.sensor_data.gsfGeoSwathPlusSpecific.side = 0;
    EXPECT_EQ(0, gsfIsStarboardPing(&d));
    d.mb_ping.sensor_data.gsfGeoSwathPlusSpecific.side = 1;
    EXPECT_EQ(1, gsfIsStarboardPing(&d));
  }
  {
    gsfRecords d{};
    ZeroStructs(d);
    d.mb_ping.sensor_id = GSF_SWATH_BATHY_SUBRECORD_KLEIN_5410_BSS_SPECIFIC;
    d.mb_ping.sensor_data.gsfKlein5410BssSpecific.side = 0;
    EXPECT_EQ(0, gsfIsStarboardPing(&d));
    d.mb_ping.sensor_data.gsfKlein5410BssSpecific.side = 1;
    EXPECT_EQ(1, gsfIsStarboardPing(&d));
  }
  {
    gsfRecords d{};
    ZeroStructs(d);
    d.mb_ping.sensor_id = GSF_SWATH_BATHY_SUBRECORD_SEABAT_SPECIFIC;
    EXPECT_EQ(0, gsfIsStarboardPing(&d));
    d.mb_ping.sensor_data.gsfSeaBatSpecific.mode = GSF_SEABAT_STBD_HEAD;
    EXPECT_EQ(1, gsfIsStarboardPing(&d));
  }
  {
    gsfRecords d{};
    ZeroStructs(d);
    d.mb_ping.sensor_id = GSF_SWATH_BATHY_SUBRECORD_ELAC_MKII_SPECIFIC;
    EXPECT_EQ(0, gsfIsStarboardPing(&d));
    d.mb_ping.sensor_data.gsfElacMkIISpecific.mode = GSF_MKII_STBD_HEAD;
    EXPECT_EQ(1, gsfIsStarboardPing(&d));
  }
  for (int sensor_id : {GSF_SWATH_BATHY_SUBRECORD_EM3000D_SPECIFIC,
                        GSF_SWATH_BATHY_SUBRECORD_EM3002D_SPECIFIC,
                        GSF_SWATH_BATHY_SUBRECORD_EM3000D_RAW_SPECIFIC,
                        GSF_SWATH_BATHY_SUBRECORD_EM3002D_RAW_SPECIFIC,
                        GSF_SWATH_BATHY_SUBRECORD_EM2040_SPECIFIC}) {
    gsfRecords d{};
    ZeroStructs(d);
    d.mb_ping.sensor_id = sensor_id;
    d.mb_ping.number_beams = 10;
    d.mb_ping.center_beam = 2;
    EXPECT_EQ(1, gsfIsStarboardPing(&d));
    d.mb_ping.center_beam = 7;
    EXPECT_EQ(0, gsfIsStarboardPing(&d));
  }
  {
    gsfRecords d{};
    ZeroStructs(d);
    d.mb_ping.sensor_id = GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC;
    d.mb_ping.sensor_data.gsfKMALLSpecific.rxTransducerInd = 0;
    EXPECT_EQ(0, gsfIsStarboardPing(&d));
    d.mb_ping.sensor_data.gsfKMALLSpecific.rxTransducerInd = 1;
    EXPECT_EQ(1, gsfIsStarboardPing(&d));
  }
  {
    gsfRecords d{};
    ZeroStructs(d);
    d.mb_ping.sensor_id = GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC;
    EXPECT_EQ(0, gsfIsStarboardPing(&d));
  }
}

TEST_F(GsfTest, GetSonarTextNameCoversAllSensorsAndModes) {
  // Expected human-readable labels specified by GSF's public API contract.
  struct NameCase {
    int sensor_id;
    int sub_id;
    const char* expected;
  };
  constexpr auto kNameCases = std::to_array<NameCase>({
      {GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC, 0, "SeaBeam"},
      {GSF_SWATH_BATHY_SUBRECORD_EM12_SPECIFIC, 0, "Simrad EM12"},
      {GSF_SWATH_BATHY_SUBRECORD_EM100_SPECIFIC, 0, "Simrad EM100"},
      {GSF_SWATH_BATHY_SUBRECORD_EM950_SPECIFIC, 0, "Simrad EM950"},
      {GSF_SWATH_BATHY_SUBRECORD_EM1000_SPECIFIC, 0, "Simrad EM1000"},
      {GSF_SWATH_BATHY_SUBRECORD_EM121A_SPECIFIC, 0, "Simrad EM121A"},
      {GSF_SWATH_BATHY_SUBRECORD_SASS_SPECIFIC, 0, "SASS"},
      {GSF_SWATH_BATHY_SUBRECORD_SEAMAP_SPECIFIC, 0, "SeaMap"},
      {GSF_SWATH_BATHY_SUBRECORD_SB_AMP_SPECIFIC, 0, "Sea Beam (w/amp)"},
      {GSF_SWATH_BATHY_SUBRECORD_SEABAT_SPECIFIC, GSF_SEABAT_9002,
       "Reson SeaBat 9002"},
      {GSF_SWATH_BATHY_SUBRECORD_SEABAT_II_SPECIFIC, GSF_SEABAT_9003,
       "Reson SeaBat 9003"},
      {GSF_SWATH_BATHY_SUBRECORD_SEABAT_II_SPECIFIC, 0, "Reson SeaBat 9001"},
      {GSF_SWATH_BATHY_SUBRECORD_SEABAT_8101_SPECIFIC, 0, "Reson SeaBat 8101"},
      {GSF_SWATH_BATHY_SUBRECORD_SEABEAM_2112_SPECIFIC, 0, "Sea Beam 2112/36"},
      {GSF_SWATH_BATHY_SUBRECORD_ELAC_MKII_SPECIFIC, 0, "ELAC MKII"},
      {GSF_SWATH_BATHY_SUBRECORD_EM120_SPECIFIC, 0, "Kongsberg EM120"},
      {GSF_SWATH_BATHY_SUBRECORD_EM120_RAW_SPECIFIC, 0, "Kongsberg EM120"},
      {GSF_SWATH_BATHY_SUBRECORD_EM300_SPECIFIC, 0, "Kongsberg EM300"},
      {GSF_SWATH_BATHY_SUBRECORD_EM1002_SPECIFIC, 0, "Kongsberg EM1002"},
      {GSF_SWATH_BATHY_SUBRECORD_EM2000_SPECIFIC, 0, "Kongsberg EM2000"},
      {GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC, 0, "Kongsberg EM3000"},
      {GSF_SWATH_BATHY_SUBRECORD_EM3000D_SPECIFIC, 0, "Kongsberg EM3000D"},
      {GSF_SWATH_BATHY_SUBRECORD_EM3002_SPECIFIC, 0, "Kongsberg EM3002"},
      {GSF_SWATH_BATHY_SUBRECORD_EM3002D_SPECIFIC, 0, "Kongsberg EM3002D"},
      {GSF_SWATH_BATHY_SUBRECORD_EM121A_SIS_SPECIFIC, 0,
       "Kongsberg EM121A (SIS)"},
      {GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC, 0, "Reson SeaBat 7125"},
      {GSF_SWATH_BATHY_SUBRECORD_RESON_TSERIES_SPECIFIC, 0,
       "Reson SeaBat T Series"},
      {GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC, 0, "Reson SeaBat 8101"},
      {GSF_SWATH_BATHY_SUBRECORD_RESON_8111_SPECIFIC, 0, "Reson SeaBat 8111"},
      {GSF_SWATH_BATHY_SUBRECORD_RESON_8124_SPECIFIC, 0, "Reson SeaBat 8124"},
      {GSF_SWATH_BATHY_SUBRECORD_RESON_8125_SPECIFIC, 0, "Reson SeaBat 8125"},
      {GSF_SWATH_BATHY_SUBRECORD_RESON_8150_SPECIFIC, 0, "Reson SeaBat 8150"},
      {GSF_SWATH_BATHY_SUBRECORD_RESON_8160_SPECIFIC, 0, "Reson SeaBat 8160"},
      {GSF_SWATH_BATHY_SUBRECORD_EM122_SPECIFIC, 0, "Kongsberg EM122"},
      {GSF_SWATH_BATHY_SUBRECORD_ME70BO_SPECIFIC, 0, "Kongsberg ME70"},
      {GSF_SWATH_BATHY_SUBRECORD_EM302_SPECIFIC, 0, "Kongsberg EM302"},
      {GSF_SWATH_BATHY_SUBRECORD_EM710_SPECIFIC, 0, "Kongsberg EM710"},
      {GSF_SWATH_BATHY_SUBRECORD_KLEIN_5410_BSS_SPECIFIC, 0, "Klein 5410"},
      {GSF_SWATH_BATHY_SUBRECORD_GEOSWATH_PLUS_SPECIFIC, 0,
       "GeoAcoustics GeoSwath+"},
      {GSF_SWATH_BATHY_SUBRECORD_EM2040_SPECIFIC, 0, "Kongsberg EM2040"},
      {GSF_SWATH_BATHY_SUBRECORD_DELTA_T_SPECIFIC, 0, "Imagenex Delta T"},
      {GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2020_SPECIFIC, 0, "R2Sonic 2020"},
      {GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2022_SPECIFIC, 0, "R2Sonic 2022"},
      {GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2024_SPECIFIC, 0, "R2Sonic 2024"},
      // Cover all 7 KMALL echoSounderID cases (upstream switch lacks default:).
      {GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC, 122, "Kongsberg EM122"},
      {GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC, 124, "Kongsberg EM124"},
      {GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC, 302, "Kongsberg EM302"},
      {GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC, 304, "Kongsberg EM304"},
      {GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC, 710, "Kongsberg EM710"},
      {GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC, 712, "Kongsberg EM712"},
      {GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC, 2040, "Kongsberg EM2040"},
      {999, 0, "Unknown"},
  });

  for (const NameCase& tc : kNameCases) {
    gsfSwathBathyPing ping{};
    ZeroStructs(ping);
    ping.sensor_id = tc.sensor_id;
    if (tc.sensor_id == GSF_SWATH_BATHY_SUBRECORD_SEABAT_SPECIFIC ||
        tc.sensor_id == GSF_SWATH_BATHY_SUBRECORD_SEABAT_II_SPECIFIC) {
      ping.sensor_data.gsfSeaBatIISpecific.mode = tc.sub_id;
    } else if (tc.sensor_id == GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC) {
      ping.sensor_data.gsfKMALLSpecific.echoSounderID = tc.sub_id;
    }
    EXPECT_STREQ(tc.expected, gsfGetSonarTextName(&ping))
        << "sensor_id=" << tc.sensor_id << " sub_id=" << tc.sub_id;
  }
}

TEST_F(GsfTest, IsNewSurveyLineDetectsHeadingTurnsAndErrors) {
  gsfRecords rec{};
  ZeroStructs(rec);
  double last_heading = -1.0;

  EXPECT_EQ(-1, gsfIsNewSurveyLine(kBadHandleZero, &rec, 5.0, &last_heading));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());
  EXPECT_EQ(-1, gsfIsNewSurveyLine(kBadHandleOver, &rec, 5.0, &last_heading));
  EXPECT_EQ(GSF_BAD_FILE_HANDLE, gsfIntError());

  const std::filesystem::path path = TestPath("survey_line.gsf");
  int h = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_CREATE, &h));
  {
    absl::Cleanup close_h = [h] { gsfClose(h); };
    std::string note = "line";
    gsfDataID cid{};
    gsfRecords crec{};
    ZeroStructs(cid, crec);
    cid.recordID = GSF_RECORD_COMMENT;
    crec.comment.comment_length = static_cast<int>(note.size());
    crec.comment.comment = note.data();
    ASSERT_GT(gsfWrite(h, &cid, &crec), 0);

    for (double heading : {10.0, 12.0, 30.0, 358.0}) {
      std::array<double, 2> depths = {50.0, 52.0};
      gsfDataID pid{};
      gsfRecords prec{};
      ZeroStructs(pid, prec);
      pid.recordID = GSF_RECORD_SWATH_BATHYMETRY_PING;
      prec.mb_ping.number_beams = 2;
      prec.mb_ping.heading = heading;
      prec.mb_ping.depth = depths.data();
      ASSERT_EQ(GSF_NORMAL,
                gsfLoadScaleFactor(&prec.mb_ping.scaleFactors,
                                   GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                                   GSF_FIELD_SIZE_TWO, 0.01, 0));
      ASSERT_GT(gsfWrite(h, &pid, &prec), 0);
    }
  }

  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_READONLY, &h));
  absl::Cleanup close_ro = [h] { gsfClose(h); };
  gsfDataID id{};
  ZeroStructs(id);

  // 1. Non-ping last_record_type returns 0.
  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(0, gsfIsNewSurveyLine(h, &rec, 5.0, &last_heading));

  // 2. First ping with negative last_heading triggers new line.
  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(1, gsfIsNewSurveyLine(h, &rec, 5.0, &last_heading));
  EXPECT_NEAR(10.0, last_heading, 0.05);

  // 3. Small heading change (2 deg <= 5 deg) stays on same line.
  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(0, gsfIsNewSurveyLine(h, &rec, 5.0, &last_heading));

  // 4. Large change (< 350 deg) starts a new survey line.
  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(1, gsfIsNewSurveyLine(h, &rec, 5.0, &last_heading));
  EXPECT_NEAR(30.0, last_heading, 0.05);

  // 5. Wrap-around crossing >= 350 deg stays on same line.
  ASSERT_GT(gsfRead(h, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  last_heading = 1.0;
  EXPECT_EQ(0, gsfIsNewSurveyLine(h, &rec, 5.0, &last_heading));
}

TEST_F(GsfTest, StringErrorPrintErrorAndIntErrorCoverAllErrorCodes) {
  absl::Cleanup reset_error = [] { gsfError = GSF_NORMAL; };

  // Expected diagnostic strings specified by GSF's public gsfStringError API.
  struct ErrorCase {
    int code;
    const char* expected;
  };
  constexpr auto kErrors = std::to_array<ErrorCase>({
      {GSF_NORMAL, "GSF Error: None"},
      {GSF_FOPEN_ERROR, "GSF Error: Unable to open requested file"},
      {GSF_UNRECOGNIZED_FILE, "GSF Error: Unrecognized file"},
      {GSF_BAD_ACCESS_MODE, "GSF Error: Illegal access mode"},
      {GSF_READ_ERROR, "GSF Error: Error occurred reading data"},
      {GSF_WRITE_ERROR, "GSF Error: Error occurred writing data"},
      {GSF_INSUFFICIENT_SIZE, "GSF Error: Insufficient size specified"},
      {GSF_RECORD_SIZE_ERROR, "GSF Error: Record size is out of bounds"},
      {GSF_CHECKSUM_FAILURE, "GSF Error: Data checksum failed"},
      {GSF_FILE_CLOSE_ERROR, "GSF Error: Error occurred closing GSF file"},
      {GSF_TOO_MANY_ARRAY_SUBRECORDS, "GSF Error: Too many array subrecords"},
      {GSF_TOO_MANY_OPEN_FILES, "GSF Error: Too many open files"},
      {GSF_MEMORY_ALLOCATION_FAILED, "GSF Error: Memory allocation failed"},
      {GSF_UNRECOGNIZED_RECORD_ID, "GSF Error: Unrecognized record id"},
      {GSF_STREAM_DECODE_FAILURE, "GSF Error: Stream decode failed"},
      {GSF_BAD_SEEK_OPTION, "GSF Error: Unrecognized file seek option"},
      {GSF_FILE_SEEK_ERROR, "GSF Error: File seek failed"},
      {GSF_UNRECOGNIZED_SENSOR_ID,
       "GSF Error: Unrecognized sensor-specific subrecord id"},
      {GSF_UNRECOGNIZED_DATA_RECORD, "GSF Error: Unrecognized data record id"},
      {GSF_UNRECOGNIZED_ARRAY_SUBRECORD_ID,
       "GSF Error: Unrecognized array subrecord id"},
      {GSF_UNRECOGNIZED_SUBRECORD_ID, "GSF Error: Unrecognized subrecord id"},
      {GSF_ILLEGAL_SCALE_FACTOR_MULTIPLIER,
       "GSF Error: Illegal scale factor multiplier specified"},
      {GSF_CANNOT_REPRESENT_PRECISION,
       "GSF Error: Can not represent requested precision"},
      {GSF_READ_TO_END_OF_FILE, "GSF Error: End of file encountered"},
      {GSF_BAD_FILE_HANDLE, "GSF Error: Bad file handle"},
      {GSF_HEADER_RECORD_DECODE_FAILED,
       "GSF Error: Decoding header record failed"},
      {GSF_MB_PING_RECORD_DECODE_FAILED,
       "GSF Error: Decoding multibeam ping record failed"},
      {GSF_SVP_RECORD_DECODE_FAILED, "GSF Error: Decoding SVP record failed"},
      {GSF_PROCESS_PARAM_RECORD_DECODE_FAILED,
       "GSF Error: Decoding processing parameters record failed"},
      {GSF_SENSOR_PARAM_RECORD_DECODE_FAILED,
       "GSF Error: Decoding sensor parameters record failed"},
      {GSF_COMMENT_RECORD_DECODE_FAILED,
       "GSF Error: Decoding comment record failed"},
      {GSF_HISTORY_RECORD_DECODE_FAILED,
       "GSF Error: Decoding history record failed"},
      {GSF_NAV_ERROR_RECORD_DECODE_FAILED,
       "GSF Error: Decoding navigation error record failed"},
      {GSF_SETVBUF_ERROR, "GSF Error: Setting internal file buffering failed"},
      {GSF_FLUSH_ERROR, "GSF Error: Flushing data buffer(s) failed"},
      {GSF_FILE_TELL_ERROR, "GSF Error: File tell failed"},
      {GSF_INDEX_FILE_OPEN_ERROR, "GSF Error: Open of index file failed"},
      {GSF_CORRUPT_INDEX_FILE_ERROR,
       "GSF Error: Index file is corrupt (delete index file)"},
      {GSF_SCALE_INDEX_CALLOC_ERROR,
       "GSF Error: Allocation of scale factor index memory failed"},
      {GSF_RECORD_TYPE_NOT_AVAILABLE,
       "GSF Error: Requested indexed record type not in GSF file"},
      {GSF_SUMMARY_RECORD_DECODE_FAILED,
       "GSF Error: Decoding summary record failed"},
      {GSF_SUMMARY_RECORD_ENCODE_FAILED,
       "GSF Error: Encoding summary record failed"},
      {GSF_INVALID_NUM_BEAMS, "GSF Error: Invalid number of beams/samples"},
      {GSF_INVALID_RECORD_NUMBER, "GSF Error: Invalid record number"},
      {GSF_INDEX_FILE_READ_ERROR, "GSF Error: Index file read error"},
      {GSF_PARAM_SIZE_FIXED,
       "GSF Error: Unable to update existing file with increased record size"},
      {GSF_SINGLE_BEAM_ENCODE_FAILED,
       "GSF Error: Encoding single beam record failed"},
      {GSF_HV_NAV_ERROR_RECORD_ENCODE_FAILED,
       "GSF Error: Encoding horizontal/vertical navigation error record failed"},
      {GSF_HV_NAV_ERROR_RECORD_DECODE_FAILED,
       "GSF Error: Decoding horizontal/vertical navigation error record failed"},
      // Upstream quirk: GSF_ATTITUDE_RECORD_ENCODE_FAILED formats "Decoding".
      {GSF_ATTITUDE_RECORD_ENCODE_FAILED,
       "GSF Error: Decoding attitude record failed"},
      {GSF_ATTITUDE_RECORD_DECODE_FAILED,
       "GSF Error: Decoding attitude record failed"},
      {GSF_OPEN_TEMP_FILE_FAILED,
       "GSF Error: Failed to open temporary file for index creation"},
      {GSF_PARTIAL_RECORD_AT_END_OF_FILE,
       "GSF Error: Corrupt/partial record at the end of the file"},
      {GSF_QUALITY_FLAGS_DECODE_ERROR,
       "GSF Error: Decoding quality flags record failed"},
      {GSF_COMPRESSION_UNSUPPORTED,
       "GSF Error: Compression method unsupported"},
      {GSF_COMPRESSION_FAILED, "GSF Error: Compression/uncompression failed"},
      {-54, "GSF Error: Unknown error"},
      {-999, "GSF Error: Unknown error"},
  });

  for (const ErrorCase& tc : kErrors) {
    gsfError = tc.code;
    EXPECT_EQ(tc.code, gsfIntError());
    EXPECT_STREQ(tc.expected, gsfStringError()) << "code=" << tc.code;
  }

  const std::filesystem::path err_path = TestPath("print_error.txt");
  std::FILE* fp = std::fopen(err_path.c_str(), "w+");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup close_fp = [fp] { std::fclose(fp); };

  gsfError = GSF_BAD_FILE_HANDLE;
  gsfPrintError(fp);
  ASSERT_EQ(0, std::fflush(fp));
  std::rewind(fp);

  std::array<char, 128> buf{};
  ASSERT_NE(nullptr, std::fgets(buf.data(), static_cast<int>(buf.size()), fp));
  EXPECT_STREQ("GSF Error: Bad file handle\n", buf.data());
}

// TODO(schwehr): Uncovered branches in src/gsf.c are limited to Windows/OS2
//   #ifdefs, calloc/stdio OS failures (GSF_MEMORY_ALLOCATION_FAILED,
//   GSF_FILE_TELL_ERROR, GSF_FILE_CLOSE_ERROR, GSF_FLUSH_ERROR,
//   GSF_SETVBUF_ERROR, GSF_READ_ERROR), the LP64-unreachable itemp >
//   MAX_GSF_SF_MULT_VALUE (ULONG_MAX) check in gsfLoadScaleFactor, and
//   remaining sibling decoders/encoders/indexers belonging in dedicated
//   gsf_*_test.cc files.

}  // namespace
}  // namespace gsf
