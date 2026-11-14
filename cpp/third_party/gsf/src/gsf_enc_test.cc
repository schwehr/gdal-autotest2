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
// Tests in-memory GSF record encoding (src/gsf_enc.c) and round-trips through
// GSF decoders or wire-format checks. Build defines in google3 set
// _LARGEFILE64_SOURCE and leave DISPLAY_SPINNER unset.
//
// See also:
//   https://github.com/schwehr/generic-sensor-format-3/blob/main/src/gsf_enc.c
//   https://github.com/schwehr/generic-sensor-format-3/blob/main/src/gsf_enc.h

#include "third_party/gsf/src/gsf.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gsf/google/util/gsf_internal.h"

namespace gsf {
namespace {

using ::testing::ElementsAre;

constexpr int kValidDecodeHandle = 1;

uint16_t ReadBe16(std::span<const unsigned char> data, size_t offset) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) |
                               static_cast<uint16_t>(data[offset + 1]));
}

int16_t ReadBeS16(std::span<const unsigned char> data, size_t offset) {
  return static_cast<int16_t>(ReadBe16(data, offset));
}

uint32_t ReadBe32(std::span<const unsigned char> data, size_t offset) {
  return (static_cast<uint32_t>(data[offset]) << 24) |
         (static_cast<uint32_t>(data[offset + 1]) << 16) |
         (static_cast<uint32_t>(data[offset + 2]) << 8) |
         static_cast<uint32_t>(data[offset + 3]);
}

int32_t ReadBeS32(std::span<const unsigned char> data, size_t offset) {
  return static_cast<int32_t>(ReadBe32(data, offset));
}

struct SubrecordSlice {
  uint8_t id = 0;
  uint32_t size = 0;
  std::span<const unsigned char> payload;
};

// Parses sequential [id:8][size:24] subrecords after the ping record header.
std::vector<SubrecordSlice> ParsePingSubrecords(
    std::span<const unsigned char> record, size_t header_bytes) {
  std::vector<SubrecordSlice> slices;
  size_t offset = header_bytes;
  while (offset < record.size()) {
    EXPECT_LE(offset + 4, record.size());
    if (offset + 4 > record.size()) break;
    const uint32_t tag = ReadBe32(record, offset);
    offset += 4;
    const uint8_t id = static_cast<uint8_t>((tag >> 24) & 0xFFu);
    const uint32_t raw_size = tag & 0x00FFFFFFu;
    uint32_t size = raw_size;
    // EncodeBRBIntensity encodes the 4-byte subrecord header inside raw_size.
    if (id == static_cast<uint8_t>(
                  GSF_SWATH_BATHY_SUBRECORD_INTENSITY_SERIES_ARRAY)) {
      EXPECT_GE(raw_size, 4u);
      if (raw_size < 4u) break;
      size = raw_size - 4u;
    }
    EXPECT_LE(offset + size, record.size());
    if (offset + size > record.size()) break;
    slices.push_back(
        {.id = id, .size = size, .payload = record.subspan(offset, size)});
    offset += size;
  }
  EXPECT_EQ(record.size(), offset);
  return slices;
}

template <typename T>
void ZeroStruct(T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  std::memset(&value, 0, sizeof(T));
}

void ResetDecodeState(GSF_FILE_TABLE& ft) {
  gsfFree(&ft.rec);
  ZeroStruct(ft);
  for (int i = 0; i < GSF_MAX_PING_ARRAY_SUBRECORDS; ++i) {
    arraySize[kValidDecodeHandle - 1][i] = 0;
  }
}

void CopyStringToFixedBuffer(std::string_view src, std::span<char> dst) {
  std::fill(dst.begin(), dst.end(), '\0');
  const size_t n = std::min(src.size(), dst.empty() ? 0 : dst.size() - 1);
  std::copy_n(src.data(), n, dst.data());
}

void CopyBytesToFixedBuffer(std::string_view src,
                            std::span<unsigned char> dst) {
  std::fill(dst.begin(), dst.end(), 0);
  const size_t n = std::min(src.size(), dst.size());
  for (size_t i = 0; i < n; ++i) {
    dst[i] = static_cast<unsigned char>(src[i]);
  }
}

void LoadScale(gsfScaleFactors& sf, unsigned int subrecord_id,
               unsigned char c_flag, double precision, int offset) {
  EXPECT_EQ(GSF_NORMAL, gsfLoadScaleFactor(&sf, subrecord_id,
                                           static_cast<char>(c_flag),
                                           precision, offset));
}

void InitBasePing(gsfSwathBathyPing& ping, short num_beams,
                  bool negative_scalars) {
  ZeroStruct(ping);
  ping.ping_time.tv_sec = 1700000100;
  ping.ping_time.tv_nsec = 250000000;
  ping.latitude = negative_scalars ? -37.7749001 : 37.7749001;
  ping.longitude = negative_scalars ? -122.4194002 : 122.4194002;
  ping.height = negative_scalars ? -12.345 : 12.345;
  ping.sep = negative_scalars ? -29.876 : 29.876;
  ping.number_beams = num_beams;
  ping.center_beam = static_cast<short>(num_beams / 2);
  ping.ping_flags = GSF_PING_USER_FLAG_01;
  ping.reserved = 7;
  ping.tide_corrector = negative_scalars ? -1.25 : 1.25;
  ping.gps_tide_corrector = negative_scalars ? -0.875 : 0.875;
  ping.depth_corrector = negative_scalars ? -3.45 : 3.45;
  ping.heading = 185.25;
  ping.pitch = negative_scalars ? -2.15 : 2.15;
  ping.roll = negative_scalars ? -3.25 : 3.25;
  ping.heave = negative_scalars ? -0.65 : 0.65;
  ping.course = 92.50;
  ping.speed = 11.75;
  ping.sensor_id = static_cast<int>(GSF_SWATH_BATHY_SUBRECORD_UNKNOWN);
}

// Holds storage for all 30 ping array pointers across tests.
struct PingBuffers {
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

  static PingBuffers Create(int num_beams, bool negative_signed) {
    PingBuffers b;
    for (int i = 0; i < num_beams; ++i) {
      const double sign = (negative_signed && (i % 2 == 0)) ? -1.0 : 1.0;
      b.depth.push_back(100.0 + i * 1.25);
      b.nominal_depth.push_back(99.5 + i * 1.25);
      b.across_track.push_back(sign * (10.5 + i * 4.25));
      b.along_track.push_back(sign * (2.0 + i * 0.5));
      b.travel_time.push_back(0.10 + i * 0.005);
      b.beam_angle.push_back(sign * (5.0 + i * 8.5));
      // Keep scaled mc_amplitude/receive_heave/incident_beam_adj/doppler_corr
      // non-negative after offset or inside signed 2-byte/compressed passes.
      b.mc_amplitude.push_back(negative_signed ? (-20.0 + i * 6.0)
                                               : (10.0 + i * 2.0));
      b.mr_amplitude.push_back(20.0 + i * 3.0);
      b.echo_width.push_back(0.01 + i * 0.002);
      b.quality_factor.push_back(1.0 + i * 0.25);
      b.receive_heave.push_back(0.10 + i * 0.05);
      b.depth_error.push_back(0.20 + i * 0.02);
      b.across_track_error.push_back(0.30 + i * 0.02);
      b.along_track_error.push_back(0.40 + i * 0.02);
      b.quality_flags.push_back(static_cast<unsigned char>(i & 0x03));
      b.beam_flags.push_back(
          static_cast<unsigned char>(i == 0 ? GSF_IGNORE_BEAM : 0));
      b.signal_to_noise.push_back(12.0 + i * 1.5);
      b.beam_angle_forward.push_back(1.0 + i * 0.5);
      b.vertical_error.push_back(0.15 + i * 0.01);
      b.horizontal_error.push_back(0.25 + i * 0.01);
      b.sector_number.push_back(static_cast<unsigned short>(i % 3));
      b.detection_info.push_back(static_cast<unsigned short>(1 + (i % 4)));
      b.incident_beam_adj.push_back(0.05 + i * 0.02);
      b.system_cleaning.push_back(static_cast<unsigned short>(i % 2));
      b.doppler_corr.push_back(0.02 + i * 0.01);
      b.sonar_vert_uncert.push_back(0.12 + i * 0.01);
      b.sonar_horz_uncert.push_back(0.22 + i * 0.01);
      b.detection_window.push_back(0.50 + i * 0.05);
      b.mean_abs_coeff.push_back(1.20 + i * 0.10);
      b.tvg_db.push_back(15.0 + i * 1.5);
    }
    return b;
  }

  void AttachAll(gsfSwathBathyPing& ping) {
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
    ping.receive_heave = receive_heave.data();
    ping.depth_error = depth_error.data();
    ping.across_track_error = across_track_error.data();
    ping.along_track_error = along_track_error.data();
    ping.quality_flags = quality_flags.data();
    ping.beam_flags = beam_flags.data();
    ping.signal_to_noise = signal_to_noise.data();
    ping.beam_angle_forward = beam_angle_forward.data();
    ping.vertical_error = vertical_error.data();
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
  }
};

void LoadAllPingScales(gsfScaleFactors& sf, unsigned char depth_size_flag,
                       unsigned char amp_size_flag,
                       unsigned char kmall_extra_size_flag,
                       unsigned char compress_flag, int mc_offset) {
  const unsigned char comp =
      static_cast<unsigned char>(compress_flag & 0x0Fu);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
            depth_size_flag | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_NOMINAL_DEPTH_ARRAY,
            depth_size_flag | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY,
            depth_size_flag | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_ALONG_TRACK_ARRAY,
            depth_size_flag | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_TRAVEL_TIME_ARRAY,
            depth_size_flag | comp, 0.001, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_BEAM_ANGLE_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_MEAN_CAL_AMPLITUDE_ARRAY,
            amp_size_flag | comp, 0.5, mc_offset);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_MEAN_REL_AMPLITUDE_ARRAY,
            amp_size_flag | comp, 0.5, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_ECHO_WIDTH_ARRAY,
            amp_size_flag | comp, 0.001, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_QUALITY_FACTOR_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_RECEIVE_HEAVE_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ERROR_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ERROR_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_ALONG_TRACK_ERROR_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_SIGNAL_TO_NOISE_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 0.5, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_BEAM_ANGLE_FORWARD_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_VERTICAL_ERROR_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_HORIZONTAL_ERROR_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_SECTOR_NUMBER_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 1.0, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_DETECTION_INFO_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 1.0, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_INCIDENT_BEAM_ADJ_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_SYSTEM_CLEANING_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 1.0, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_DOPPLER_CORRECTION_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_SONAR_VERT_UNCERT_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_SONAR_HORZ_UNCERT_ARRAY,
            GSF_FIELD_SIZE_DEFAULT | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_DETECTION_WINDOW_ARRAY,
            kmall_extra_size_flag | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_MEAN_ABS_COEF_ARRAY,
            kmall_extra_size_flag | comp, 0.01, 0);
  LoadScale(sf, GSF_SWATH_BATHY_SUBRECORD_TVG_ARRAY,
            kmall_extra_size_flag | comp, 0.1, 0);
}

void PopulateEM3RawSpecific(t_gsfEM3RawSpecific& em3, int runtime_model,
                            bool negative_signed) {
  const double sign = negative_signed ? -1.0 : 1.0;
  em3.model_number = runtime_model;
  em3.ping_counter = 101;
  em3.serial_number = 555;
  em3.surface_velocity = 1505.2;
  em3.transducer_depth = 3.25;
  em3.valid_detections = 64;
  em3.sampling_frequency = 25000.25;
  em3.vehicle_depth = 10.5;
  em3.depth_difference = 0.75;
  em3.offset_multiplier = 1;
  em3.transmit_sectors = 2;
  for (int i = 0; i < em3.transmit_sectors; ++i) {
    em3.sector[i].tilt_angle = sign * (1.5 + i);
    em3.sector[i].focus_range = 20.0 + i;
    em3.sector[i].signal_length = 0.002;
    em3.sector[i].transmit_delay = 0.001 * i;
    em3.sector[i].center_frequency = 30000.0;
    em3.sector[i].waveform_id = GSF_EM_WAVEFORM_FM_UP;
    em3.sector[i].sector_number = i;
    em3.sector[i].signal_bandwidth = 1200.0;
  }
  em3.run_time.model_number = runtime_model;
  em3.run_time.dg_time.tv_sec = 1700000100;
  em3.run_time.dg_time.tv_nsec = 500000000;
  em3.run_time.ping_counter = 101;
  em3.run_time.serial_number = 555;
  em3.run_time.min_depth = 10.0;
  em3.run_time.max_depth = 500.0;
  em3.run_time.absorption = 8.25;
  em3.run_time.tx_pulse_length = 200.0;
  em3.run_time.tx_beam_width = 1.5;
  em3.run_time.tx_power_re_max = 3.0;
  em3.run_time.rx_beam_width = 1.8;
  em3.run_time.rx_bandwidth = 150.0;
  em3.run_time.rx_fixed_gain = 4.0;
  em3.run_time.tvg_cross_over_angle = 12.0;
  em3.run_time.max_port_swath_width = 300;
  em3.run_time.max_stbd_swath_width = 300;
  em3.run_time.durotong_speed = 1495.5;
  em3.run_time.tx_along_tilt = sign * 2.25;
  em3.run_time.hi_low_absorption_ratio = 2.0;
  em3.pu_status.pu_cpu_load = 25.0;
  em3.pu_status.sensor_status = GSF_EM_PU_ACTIVE | GSF_EM_VALID_POSITION;
  em3.pu_status.achieved_port_coverage = 60;
  em3.pu_status.achieved_stbd_coverage = 60;
  em3.pu_status.yaw_stabilization = sign * 1.5;
}

void PopulateEM4Specific(t_gsfEM4Specific& em4, int model,
                         bool negative_signed) {
  const double sign = negative_signed ? -1.0 : 1.0;
  em4.model_number = model;
  em4.ping_counter = 202;
  em4.serial_number = 777;
  em4.surface_velocity = 1510.4;
  em4.transducer_depth = 4.15;
  em4.valid_detections = 128;
  em4.sampling_frequency = 32768.625;
  em4.doppler_corr_scale = 100;
  em4.vehicle_depth = 15.25;
  em4.transmit_sectors = 2;
  for (int i = 0; i < em4.transmit_sectors; ++i) {
    em4.sector[i].tilt_angle = sign * (2.5 + i);
    em4.sector[i].focus_range = 30.0 + i;
    em4.sector[i].signal_length = 0.003;
    em4.sector[i].transmit_delay = 0.001 * i;
    em4.sector[i].center_frequency = 70000.0;
    em4.sector[i].mean_absorption = 18.5;
    em4.sector[i].waveform_id = GSF_EM_WAVEFORM_CW;
    em4.sector[i].sector_number = i;
    em4.sector[i].signal_bandwidth = 2400.0;
  }
  em4.run_time.model_number = model;
  em4.run_time.dg_time.tv_sec = 1700000200;
  em4.run_time.ping_counter = 202;
  em4.run_time.serial_number = 777;
  em4.run_time.min_depth = 5.0;
  em4.run_time.max_depth = 800.0;
  em4.run_time.absorption = 12.5;
  em4.run_time.tx_pulse_length = 150.0;
  em4.run_time.tx_beam_width = 1.0;
  em4.run_time.tx_power_re_max = 2.0;
  em4.run_time.rx_beam_width = 1.2;
  em4.run_time.rx_bandwidth = 200.0;
  em4.run_time.rx_fixed_gain = 6.0;
  em4.run_time.tvg_cross_over_angle = 15.0;
  em4.run_time.ssv_source = 1;
  em4.run_time.max_port_swath_width = 400;
  em4.run_time.beam_spacing = 2;
  em4.run_time.max_port_coverage = 65;
  em4.run_time.stabilization = 3;
  em4.run_time.max_stbd_coverage = 65;
  em4.run_time.max_stbd_swath_width = 400;
  em4.run_time.tx_along_tilt = sign * 1.75;
  em4.run_time.filter_id_2 = 1;
  em4.pu_status.pu_cpu_load = 35.0;
  em4.pu_status.sensor_status = GSF_EM_PU_ACTIVE;
  em4.pu_status.achieved_port_coverage = 65;
  em4.pu_status.achieved_stbd_coverage = 65;
  em4.pu_status.yaw_stabilization = sign * 0.75;
}

void PopulateEM3Specific(t_gsfEM3Specific& em3) {
  em3.model_number = 3000;
  em3.ping_number = 42;
  em3.serial_number = 1234;
  em3.surface_velocity = 1500.5;
  em3.transducer_depth = 4.5;
  em3.valid_beams = 127;
  em3.sample_rate = 10000;
  em3.depth_difference = 0.25;
  em3.offset_multiplier = 1;
  em3.run_time[0].model_number = 3000;
  em3.run_time[0].dg_time.tv_sec = 1700000100;
  em3.run_time[0].dg_time.tv_nsec = 123000000;
  em3.run_time[0].ping_number = 42;
  em3.run_time[0].serial_number = 1234;
  em3.run_time[0].system_status = 1;
  em3.run_time[0].mode = 2;
  em3.run_time[0].filter_id = 3;
  em3.run_time[0].min_depth = 10.0;
  em3.run_time[0].max_depth = 400.0;
  em3.run_time[0].absorption = 9.5;
  em3.run_time[0].pulse_length = 150.0;
  em3.run_time[0].transmit_beam_width = 1.5;
  em3.run_time[0].power_reduction = 3;
  em3.run_time[0].receive_beam_width = 1.6;
  em3.run_time[0].receive_bandwidth = 150;
  em3.run_time[0].receive_gain = 4;
  em3.run_time[0].cross_over_angle = 12;
  em3.run_time[0].ssv_source = 1;
  em3.run_time[0].swath_width = 600;
  em3.run_time[0].beam_spacing = 1;
  em3.run_time[0].coverage_sector = 120;
  em3.run_time[0].stabilization = 3;
  em3.run_time[0].port_swath_width = 300;
  em3.run_time[0].stbd_swath_width = 300;
  em3.run_time[0].port_coverage_sector = 60;
  em3.run_time[0].stbd_coverage_sector = 60;
  em3.run_time[0].hilo_freq_absorp_ratio = 2;
}

void PopulateKMALLSpecific(t_gsfKMALLSpecific& kmall, bool negative_signed,
                           bool high_freq_mode) {
  const double sign = negative_signed ? -1.0 : 1.0;
  kmall.gsfKMALLVersion = 1;
  kmall.dgmType = GSF_KMALL_MRZ;
  kmall.dgmVersion = 1;
  kmall.systemID = 1;
  kmall.echoSounderID = 2040;
  kmall.numBytesCmnPart = 12;
  kmall.pingCnt = 505;
  kmall.rxFansPerPing = 1;
  kmall.rxFanIndex = 0;
  kmall.swathsPerPing = 1;
  kmall.swathAlongPosition = 0;
  kmall.txTransducerInd = 0;
  kmall.rxTransducerInd = 0;
  kmall.numRxTransducers = 1;
  kmall.algorithmType = 0;
  kmall.numBytesInfoData = 140;
  kmall.pingRate_Hz = 4.5;
  kmall.beamSpacing = 2;
  kmall.depthMode = 1;
  kmall.subDepthMode = 0;
  kmall.distanceBtwSwath = 100;
  kmall.detectionMode = 0;
  kmall.pulseForm = 2;
  kmall.frequencyMode_Hz = high_freq_mode ? 45000.2 : -1.0;
  kmall.freqRangeLowLim_Hz = 30000.0;
  kmall.freqRangeHighLim_Hz = 95000.0;
  kmall.maxTotalTxPulseLength_sec = 0.005;
  kmall.maxEffTxPulseLength_sec = 0.004;
  kmall.maxEffTxBandWidth_Hz = 4000.0;
  kmall.absCoeff_dBPerkm = 14.25;
  kmall.portSectorEdge_deg = sign * 65.0;
  kmall.starbSectorEdge_deg = sign * 65.0;
  kmall.portMeanCov_deg = sign * 62.5;
  kmall.starbMeanCov_deg = sign * 62.5;
  kmall.portMeanCov_m = sign * 120.0;
  kmall.starbMeanCov_m = sign * 120.0;
  kmall.modeAndStabilisation = 0x03;
  kmall.runtimeFilter1 = 0x01;
  kmall.runtimeFilter2 = 0x02;
  kmall.pipeTrackingStatus = 1;
  kmall.transmitArraySizeUsed_deg = 0.8;
  kmall.receiveArraySizeUsed_deg = 1.0;
  kmall.transmitPower_dB = sign * 10.0;
  kmall.SLrampUpTimeRemaining = 0;
  kmall.yawAngle_deg = sign * 1.5;
  kmall.numTxSectors = 2;
  kmall.numBytesPerTxSector = 48;
  kmall.headingVessel_deg = sign * 45.0;
  kmall.soundSpeedAtTxDepth_mPerSec = sign * 1502.3;
  kmall.txTransducerDepth_m = sign * 4.2;
  kmall.z_waterLevelReRefPoint_m = sign * 0.5;
  kmall.x_kmallToall_m = sign * 0.1;
  kmall.y_kmallToall_m = sign * 0.2;
  kmall.latLongInfo = 1;
  kmall.posSensorStatus = 0;
  kmall.attitudeSensorStatus = 0;
  kmall.latitude_deg = sign * 36.8;
  kmall.longitude_deg = sign * 121.9;
  kmall.ellipsoidHeightReRefPoint_m = sign * 1.25;
  for (int i = 0; i < kmall.numTxSectors; ++i) {
    kmall.sector[i].txSectorNumb = i;
    kmall.sector[i].txArrNumber = 0;
    kmall.sector[i].txSubArray = i;
    kmall.sector[i].sectorTransmitDelay_sec = 0.001 * i;
    kmall.sector[i].tiltAngleReTx_deg = sign * (1.2 + i);
    kmall.sector[i].txNominalSourceLevel_dB = sign * 215.0;
    kmall.sector[i].txFocusRange_m = 50.0;
    kmall.sector[i].centreFreq_Hz = 70000.0;
    kmall.sector[i].signalBandWidth_Hz = 2500.0;
    kmall.sector[i].totalSignalLength_sec = 0.003;
    kmall.sector[i].pulseShading = 50;
    kmall.sector[i].signalWaveForm = GSF_EM_WAVEFORM_FM_DOWN;
    kmall.sector[i].highVoltageLevel_dB = negative_signed ? -3.5f : 3.5f;
    kmall.sector[i].sectorTrackiongCorr_dB = negative_signed ? -0.25f : 0.25f;
    kmall.sector[i].effectiveSignalLength_sec =
        negative_signed ? -0.002f : 0.002f;
  }
  kmall.numBytesRxInfo = 32;
  kmall.numSoundingsMaxMain = 400;
  kmall.numSoundingsValidMain = 398;
  kmall.numBytesPerSounding = 120;
  kmall.WCSampleRate = 25000.5;
  kmall.seabedImageSampleRate = 12500.25;
  kmall.BSnormal_dB = sign * 18.5;
  kmall.BSoblique_dB = sign * 24.5;
  kmall.extraDetectionAlarmFlag = 0;
  kmall.numExtraDetections = 3;
  kmall.numExtraDetectionClasses = 2;
  kmall.numBytesPerClass = 4;
  for (int i = 0; i < kmall.numExtraDetectionClasses; ++i) {
    kmall.extraDetClassInfo[i].numExtraDetInClass = i + 1;
    kmall.extraDetClassInfo[i].alarmFlag = i % 2;
  }
}

void PopulateGeoSwathPlusSpecific(t_gsfGeoSwathPlusSpecific& geo) {
  geo.data_source = 1;
  geo.side = 0;
  geo.model_number = 250;
  geo.frequency = 250000.0;
  geo.echosounder_type = 1;
  geo.ping_number = 1001;
  geo.num_nav_samples = 2;
  geo.num_attitude_samples = 4;
  geo.num_heading_samples = 2;
  geo.num_miniSVS_samples = 1;
  geo.num_echosounder_samples = 1;
  geo.num_raa_samples = 64;
  geo.mean_sv = 1500.0;
  geo.surface_velocity = 1501.0;
  geo.valid_beams = 64;
  geo.sample_rate = 100000.0;
  geo.pulse_length = 64.0;
  geo.ping_length = 100;
  geo.transmit_power = 2;
  geo.sidescan_gain_channel = 1;
  geo.stabilization = 1;
  geo.gps_quality = 4;
  geo.range_uncertainty = 0.05;
  geo.angle_uncertainty = 0.1;
}

void PopulateKlein5410BssSpecific(t_gsfKlein5410BssSpecific& klein) {
  klein.data_source = 0;
  klein.side = 1;
  klein.model_number = 5410;
  klein.acoustic_frequency = 455000.0;
  klein.sampling_frequency = 22750.0;
  klein.ping_number = 808;
  klein.num_samples = 1024;
  klein.num_raa_samples = 128;
  klein.error_flags = 0;
  klein.range = 75;
  klein.fish_depth = 12.345;
  klein.fish_altitude = 15.5;
  klein.sound_speed = 1498.75;
  klein.tx_waveform = 1;
  klein.altimeter = 1;
  klein.raw_data_config = 3;
}

void PopulateReson8100Specific(t_gsfReson8100Specific& r81, int model) {
  r81.latency = 5;
  r81.ping_number = 333;
  r81.sonar_id = 0x12345678;
  r81.sonar_model = model;
  r81.frequency = 240;
  r81.surface_velocity = 1502.5;
  r81.sample_rate = 30000;
  r81.ping_rate = 15000;
  r81.mode = GSF_8100_AMPLITUDE | GSF_8100_PITCH_STAB | GSF_8100_ROLL_STAB;
  r81.range = 100;
  r81.power = 6;
  r81.gain = 20;
  r81.pulse_width = 100;
  r81.tvg_spreading = 80;
  r81.tvg_absorption = 35;
  r81.fore_aft_bw = 1.5;
  r81.athwart_bw = 1.5;
  r81.projector_type = 1;
  r81.projector_angle = 0;
  r81.range_filt_min = 2.0;
  r81.range_filt_max = 120.0;
  r81.depth_filt_min = 1.0;
  r81.depth_filt_max = 100.0;
  r81.filters_active = 3;
  r81.temperature = 145;
  r81.beam_spacing = 0.75;
}

void PopulateReson7100Specific(t_gsfReson7100Specific& r71) {
  r71.protocol_version = 5;
  r71.device_id = 7125;
  r71.major_serial_number = 1;
  r71.minor_serial_number = 42;
  r71.ping_number = 444;
  r71.multi_ping_seq = 0;
  r71.frequency = 400000.0;
  r71.sample_rate = 34000.0;
  r71.receiver_bandwdth = 12000.0;
  r71.tx_pulse_width = 0.0001;
  r71.tx_pulse_type_id = 0;
  r71.tx_pulse_envlp_id = 1;
  r71.tx_pulse_envlp_param = 0.5;
  r71.max_ping_rate = 25.0;
  r71.ping_period = 0.05;
  r71.range = 75.0;
  r71.power = 210.0;
  r71.gain = 18.0;
  r71.control_flags = 0x01;
  r71.projector_id = 2;
  r71.projector_steer_angl_vert = 0.0;
  r71.projector_steer_angl_horz = 0.0;
  r71.projector_beam_wdth_vert = 1.0;
  r71.projector_beam_wdth_horz = 2.0;
  r71.projector_beam_focal_pt = 25.0;
  r71.projector_beam_weighting_window_type = 1;
  r71.projector_beam_weighting_window_param = 2;
  r71.transmit_flags = GSF_7100_PITCH_STAB;
  r71.hydrophone_id = 3;
  r71.receiving_beam_weighting_window_type = 1;
  r71.receiving_beam_weighting_window_param = 1;
  r71.receive_flags = GSF_7100_ROLL_STAB;
  r71.receive_beam_width = 0.5;
  r71.range_filt_min = 1.0;
  r71.range_filt_max = 100.0;
  r71.depth_filt_min = 1.0;
  r71.depth_filt_max = 90.0;
  r71.absorption = 110.0;
  r71.sound_velocity = 1500.5;
  r71.spreading = 20.0;
  r71.raw_data_from_7027 = 1;
  r71.sv_source = 0;
  r71.layer_comp_flag = 1;
}

void PopulateResonTSeriesSpecific(t_gsfResonTSeriesSpecific& rt,
                                  bool negative_signed) {
  const double sign = negative_signed ? -1.0 : 1.0;
  rt.protocol_version = 5;
  rt.device_id = 50;
  rt.number_devices = 1;
  rt.system_enumerator = 0;
  rt.major_serial_number = 2;
  rt.minor_serial_number = 55;
  rt.ping_number = 606;
  rt.multi_ping_seq = 1;
  rt.frequency = 300000.0;
  rt.sample_rate = 65000.0;
  rt.receiver_bandwdth = 20000.0;
  rt.tx_pulse_width = 0.0002;
  rt.tx_pulse_type_id = 1;
  rt.tx_pulse_envlp_id = 1;
  rt.tx_pulse_envlp_param = 0.4;
  rt.tx_pulse_mode = 1;
  rt.max_ping_rate = 20.0;
  rt.ping_period = 0.05;
  rt.range = 80.0;
  rt.power = 12.0;
  rt.gain = 6.0;
  rt.control_flags = 0x10;
  rt.projector_id = 1;
  rt.projector_steer_angl_vert = 1.5;
  rt.projector_steer_angl_horz = 0.5;
  rt.projector_beam_wdth_vert = 1.0;
  rt.projector_beam_wdth_horz = 1.0;
  rt.projector_beam_focal_pt = 20.0;
  rt.receive_beam_width = 0.6;
  rt.range_filt_min = 1.0;
  rt.range_filt_max = 100.0;
  rt.depth_filt_min = negative_signed ? -2.5 : 2.5;
  rt.depth_filt_max = 95.0;
  rt.absorption = 95.0;
  rt.sound_velocity = 1501.2;
  rt.sv_source = 0;
  rt.spreading = 20.0;
  rt.beam_spacing_mode = 2;
  rt.sonar_source_mode = 0;
  rt.coverage_mode = 1;
  rt.coverage_angle = 130.0;
  rt.horizontal_receiver_steering_angle = sign * 2.5;
  rt.uncertainty_type = 1;
  rt.transmitter_steering_angle = sign * 0.02;
  rt.applied_roll = sign * 0.01;
  rt.detection_algorithm = 1;
  rt.detection_flags = 0x03;
  CopyStringToFixedBuffer("T50-P", rt.device_description);
  rt.match_filter_control = 1;
  rt.match_filter_start_freq = 280000.0;
  rt.match_filter_end_freq = 320000.0;
  rt.match_filter_window_type = 2;
  rt.match_filter_shading_value = 0.5;
  rt.match_filter_effect_pulse_width = 0.00005;
}

void PopulateDeltaTSpecific(t_gsfDeltaTSpecific& dt) {
  CopyStringToFixedBuffer("83P",
                          std::span<char>(dt.decode_file_type,
                                          sizeof(dt.decode_file_type)));
  dt.version = 4;
  dt.ping_byte_size = 512;
  dt.interrogation_time.tv_sec = 1700000300;
  dt.interrogation_time.tv_nsec = 400000000;
  dt.samples_per_beam = 250;
  dt.sector_size = 120.0;
  dt.start_angle = -60.0;
  dt.angle_increment = 0.5;
  dt.acoustic_range = 60;
  dt.acoustic_frequency = 260;
  dt.sound_velocity = 1495.5;
  dt.range_resolution = 2.0;
  dt.profile_tilt_angle = -5.0;
  dt.repetition_rate = 100.0;
  dt.ping_number = 707;
  dt.intensity_flag = 1;
  dt.ping_latency = 0.01;
  dt.data_latency = 0.02;
  dt.sample_rate_flag = 1;
  dt.option_flags = 3;
  dt.num_pings_avg = 1;
  dt.center_ping_time_offset = 0.005;
  dt.user_defined_byte = 9;
  dt.altitude = 18.5;
  dt.external_sensor_flags = 0x0F;
  dt.pulse_length = 0.0001;
  dt.fore_aft_beamwidth = 1.5;
  dt.athwartships_beamwidth = 0.75;
}

void PopulateR2SonicSpecific(t_gsfR2SonicSpecific& r2s) {
  CopyBytesToFixedBuffer("2024", r2s.model_number);
  CopyBytesToFixedBuffer("100017", r2s.serial_number);
  r2s.dg_time.tv_sec = 1700000400;
  r2s.dg_time.tv_nsec = 750000000;
  r2s.ping_number = 909;
  r2s.ping_period = 0.08;
  r2s.sound_speed = 1503.2;
  r2s.frequency = 400000.0;
  r2s.tx_power = 215.0;
  r2s.tx_pulse_width = 0.00015;
  r2s.tx_beamwidth_vert = 1.0;
  r2s.tx_beamwidth_horiz = 0.5;
  r2s.tx_steering_vert = 1.25;
  r2s.tx_steering_horiz = 0.5;
  r2s.tx_misc_info = 1;
  r2s.rx_bandwidth = 50000.0;
  r2s.rx_sample_rate = 60000.0;
  r2s.rx_range = 80.0;
  r2s.rx_gain = 14.0;
  r2s.rx_spreading = 20.0;
  r2s.rx_absorption = 105.0;
  r2s.rx_mount_tilt = 2.0;
  r2s.rx_misc_info = 2;
  r2s.num_beams = 256;
  for (int i = 0; i < 6; ++i) {
    r2s.A0_more_info[i] = 0.1 * (i + 1);
    r2s.A2_more_info[i] = 0.2 * (i + 1);
  }
  r2s.G0_depth_gate_min = 0.01;
  r2s.G0_depth_gate_max = 0.25;
  r2s.G0_depth_gate_slope = 0.5;
}

std::vector<unsigned char> MakeEncodeBuffer() {
  return std::vector<unsigned char>(GSF_MAX_RECORD_SIZE, 0);
}

TEST(GsfEncTest, EncodeHeaderSetsVersionAndRoundTrips) {
  auto buf = MakeEncodeBuffer();
  gsfHeader header{};
  ZeroStruct(header);
  const int encoded = gsfEncodeHeader(buf.data(), &header);
  EXPECT_EQ(GSF_VERSION_SIZE, encoded);
  EXPECT_STREQ(GSF_VERSION, header.version);

  gsfHeader decoded{};
  ZeroStruct(decoded);
  EXPECT_EQ(static_cast<int>(std::strlen(GSF_VERSION)),
            gsfDecodeHeader(&decoded, buf.data()));
  EXPECT_STREQ(GSF_VERSION, decoded.version);
}

TEST(GsfEncTest, EncodeSwathBathySummaryPositiveAndNegativeRoundTrips) {
  auto buf = MakeEncodeBuffer();
  // 1. Positive summary extents round-trip.
  {
    gsfSwathBathySummary sum{};
    ZeroStruct(sum);
    sum.start_time = {1700000000, 123000000};
    sum.end_time = {1700000500, 456000000};
    sum.min_latitude = 10.5;
    sum.min_longitude = 20.25;
    sum.max_latitude = 15.75;
    sum.max_longitude = 25.5;
    sum.min_depth = 50.25;
    sum.max_depth = 1234.56;

    const int encoded = gsfEncodeSwathBathySummary(buf.data(), &sum);
    EXPECT_EQ(40, encoded);

    gsfSwathBathySummary decoded{};
    ZeroStruct(decoded);
    EXPECT_EQ(encoded, gsfDecodeSwathBathySummary(&decoded, buf.data()));
    EXPECT_EQ(sum.start_time.tv_sec, decoded.start_time.tv_sec);
    EXPECT_EQ(sum.start_time.tv_nsec, decoded.start_time.tv_nsec);
    EXPECT_EQ(sum.end_time.tv_sec, decoded.end_time.tv_sec);
    EXPECT_EQ(sum.end_time.tv_nsec, decoded.end_time.tv_nsec);
    EXPECT_NEAR(sum.min_latitude, decoded.min_latitude, 1e-6);
    EXPECT_NEAR(sum.min_longitude, decoded.min_longitude, 1e-6);
    EXPECT_NEAR(sum.max_latitude, decoded.max_latitude, 1e-6);
    EXPECT_NEAR(sum.max_longitude, decoded.max_longitude, 1e-6);
    EXPECT_NEAR(sum.min_depth, decoded.min_depth, 0.01);
    EXPECT_NEAR(sum.max_depth, decoded.max_depth, 0.01);
  }
  // 2. Negative summary extents and wire max_depth check.
  {
    std::fill(buf.begin(), buf.begin() + 64, 0);
    gsfSwathBathySummary sum{};
    ZeroStruct(sum);
    sum.start_time = {1700000100, 0};
    sum.end_time = {1700000200, 0};
    sum.min_latitude = -45.1234567;
    sum.min_longitude = -122.7654321;
    sum.max_latitude = -10.5;
    sum.max_longitude = -100.25;
    sum.min_depth = -5.5;
    sum.max_depth = -1.25;

    const int encoded = gsfEncodeSwathBathySummary(buf.data(), &sum);
    EXPECT_EQ(40, encoded);

    gsfSwathBathySummary decoded{};
    ZeroStruct(decoded);
    EXPECT_EQ(encoded, gsfDecodeSwathBathySummary(&decoded, buf.data()));
    EXPECT_NEAR(sum.min_latitude, decoded.min_latitude, 1e-6);
    EXPECT_NEAR(sum.min_longitude, decoded.min_longitude, 1e-6);
    EXPECT_NEAR(sum.max_latitude, decoded.max_latitude, 1e-6);
    EXPECT_NEAR(sum.max_longitude, decoded.max_longitude, 1e-6);
    EXPECT_NEAR(sum.min_depth, decoded.min_depth, 0.01);
    // gsfDecodeSwathBathySummary omits signed cast on max_depth; check wire.
    EXPECT_EQ(-125, ReadBeS32(buf, 36));
  }
}

TEST(GsfEncTest, EncodeSinglebeamAllSensorsAndInvalidSensor) {
  auto buf = MakeEncodeBuffer();
  struct SinglebeamCase {
    int sensor_id;
    int expected_bytes;
    bool negative_fields = false;
  };
  constexpr auto kSinglebeamCases = std::to_array<SinglebeamCase>({
      {.sensor_id = static_cast<int>(GSF_SINGLE_BEAM_SUBRECORD_UNKNOWN),
       .expected_bytes = 42},
      {.sensor_id =
           static_cast<int>(GSF_SINGLE_BEAM_SUBRECORD_ECHOTRAC_SPECIFIC),
       .expected_bytes = 46,
       .negative_fields = true},
      {.sensor_id =
           static_cast<int>(GSF_SINGLE_BEAM_SUBRECORD_BATHY2000_SPECIFIC),
       .expected_bytes = 46},
      {.sensor_id = static_cast<int>(GSF_SINGLE_BEAM_SUBRECORD_MGD77_SPECIFIC),
       .expected_bytes = 56},
      {.sensor_id = static_cast<int>(GSF_SINGLE_BEAM_SUBRECORD_BDB_SPECIFIC),
       .expected_bytes = 52,
       .negative_fields = true},
      {.sensor_id = static_cast<int>(GSF_SINGLE_BEAM_SUBRECORD_NOSHDB_SPECIFIC),
       .expected_bytes = 46},
      {.sensor_id = static_cast<int>(GSF_SINGLE_BEAM_SUBRECORD_UNKNOWN),
       .expected_bytes = 42,
       .negative_fields = true},
  });

  for (const auto& tc : kSinglebeamCases) {
    std::fill(buf.begin(), buf.begin() + 128, 0);
    gsfSingleBeamPing ping{};
    ZeroStruct(ping);
    const double sign = tc.negative_fields ? -1.0 : 1.0;
    ping.ping_time = {1700000100, 500000000};
    ping.latitude = sign * 35.125;
    ping.longitude = sign * 120.375;
    ping.tide_corrector = sign * 1.15;
    ping.depth_corrector = sign * 2.25;
    ping.heading = 90.5;
    ping.pitch = sign * 1.75;
    ping.roll = sign * 2.50;
    ping.heave = sign * 0.45;
    ping.depth = sign * 42.75;
    ping.sound_speed_correction = sign * 0.35;
    ping.positioning_system_type = 4;
    ping.sensor_id = tc.sensor_id;

    switch (tc.sensor_id) {
      case GSF_SINGLE_BEAM_SUBRECORD_ECHOTRAC_SPECIFIC:
        ping.sensor_data.gsfEchotracSpecific.navigation_error = 12;
        ping.sensor_data.gsfEchotracSpecific.mpp_source =
            GSF_SB_MPP_SOURCE_DGPS_TRIMBLE;
        ping.sensor_data.gsfEchotracSpecific.tide_source = 2;
        break;
      case GSF_SINGLE_BEAM_SUBRECORD_BATHY2000_SPECIFIC:
        ping.sensor_data.gsfBathy2000Specific.navigation_error = 8;
        ping.sensor_data.gsfBathy2000Specific.mpp_source =
            GSF_SB_MPP_SOURCE_GPS_3S;
        ping.sensor_data.gsfBathy2000Specific.tide_source = 1;
        break;
      case GSF_SINGLE_BEAM_SUBRECORD_MGD77_SPECIFIC:
        ping.sensor_data.gsfMGD77Specific.time_zone_corr = 5;
        ping.sensor_data.gsfMGD77Specific.position_type_code = 1;
        ping.sensor_data.gsfMGD77Specific.correction_code = 2;
        ping.sensor_data.gsfMGD77Specific.bathy_type_code = 3;
        ping.sensor_data.gsfMGD77Specific.quality_code = 4;
        ping.sensor_data.gsfMGD77Specific.travel_time = 0.0567;
        break;
      case GSF_SINGLE_BEAM_SUBRECORD_BDB_SPECIFIC:
        ping.sensor_data.gsfBDBSpecific.doc_no = 12345;
        ping.sensor_data.gsfBDBSpecific.eval = '1';
        ping.sensor_data.gsfBDBSpecific.classification = 'U';
        ping.sensor_data.gsfBDBSpecific.track_adj_flag = 'Y';
        ping.sensor_data.gsfBDBSpecific.source_flag = 'S';
        ping.sensor_data.gsfBDBSpecific.pt_or_track_ln = 'T';
        ping.sensor_data.gsfBDBSpecific.datum_flag = 'W';
        break;
      case GSF_SINGLE_BEAM_SUBRECORD_NOSHDB_SPECIFIC:
        ping.sensor_data.gsfNOSHDBSpecific.type_code = 7;
        ping.sensor_data.gsfNOSHDBSpecific.carto_code = 11;
        break;
      default:
        break;
    }

    const int encoded = gsfEncodeSinglebeam(buf.data(), &ping);
    EXPECT_EQ(tc.expected_bytes, encoded);

    gsfSingleBeamPing decoded{};
    ZeroStruct(decoded);
    EXPECT_EQ(encoded, gsfDecodeSinglebeam(&decoded, buf.data(), encoded));
    EXPECT_EQ(tc.sensor_id, decoded.sensor_id);
    EXPECT_NEAR(ping.latitude, decoded.latitude, 1e-6);
    EXPECT_NEAR(ping.longitude, decoded.longitude, 1e-6);
    EXPECT_NEAR(ping.depth, decoded.depth, 0.01);
  }

  gsfSingleBeamPing invalid{};
  ZeroStruct(invalid);
  invalid.sensor_id = 999;
  gsfError = GSF_NORMAL;
  EXPECT_EQ(-1, gsfEncodeSinglebeam(buf.data(), &invalid));
  EXPECT_EQ(GSF_UNRECOGNIZED_SENSOR_ID, gsfIntError());
}

TEST(GsfEncTest, EncodeSoundVelocityProfileRoundTrip) {
  auto buf = MakeEncodeBuffer();
  GSF_FILE_TABLE ft{};
  ZeroStruct(ft);
  absl::Cleanup cleanup([&ft] { ResetDecodeState(ft); });

  std::array<double, 3> depths = {0.0, 250.5, 1000.25};
  std::array<double, 3> speeds = {1520.5, 1495.25, 1488.75};

  for (const bool neg : {false, true}) {
    std::fill(buf.begin(), buf.begin() + 128, 0);
    gsfSVP svp{};
    ZeroStruct(svp);
    svp.observation_time = {1700000100, 100000000};
    svp.application_time = {1700000200, 200000000};
    svp.latitude = neg ? -25.5 : 25.5;
    svp.longitude = neg ? -110.25 : 110.25;
    svp.number_points = static_cast<int>(depths.size());
    svp.depth = depths.data();
    svp.sound_speed = speeds.data();

    const int encoded = gsfEncodeSoundVelocityProfile(buf.data(), &svp);
    EXPECT_EQ(28 + 3 * 8, encoded);

    gsfSVP decoded{};
    ZeroStruct(decoded);
    EXPECT_EQ(encoded, gsfDecodeSoundVelocityProfile(&decoded, &ft, buf.data()));
    EXPECT_EQ(3, decoded.number_points);
    EXPECT_NEAR(svp.latitude, decoded.latitude, 1e-6);
    EXPECT_NEAR(svp.longitude, decoded.longitude, 1e-6);
    EXPECT_NEAR(depths[2], decoded.depth[2], 0.01);
    EXPECT_NEAR(speeds[1], decoded.sound_speed[1], 0.01);
  }
}

TEST(GsfEncTest, EncodeProcessingParametersRoundTripAndNullEntry) {
  auto buf = MakeEncodeBuffer();
  GSF_FILE_TABLE ft{};
  ZeroStruct(ft);
  absl::Cleanup cleanup([&ft] { ResetDecodeState(ft); });

  std::string p0 = "FILTER=MEDIAN";
  std::string p1 = "TIDE=OBSERVED";
  gsfProcessingParameters param{};
  ZeroStruct(param);
  param.param_time = {1700000300, 300000000};
  param.number_parameters = 2;
  param.param[0] = p0.data();
  param.param[1] = p1.data();

  const int encoded = gsfEncodeProcessingParameters(buf.data(), &param);
  EXPECT_EQ(10 + 2 + static_cast<int>(p0.size() + 1) + 2 +
                static_cast<int>(p1.size() + 1),
            encoded);
  EXPECT_EQ(static_cast<short>(p0.size() + 1), param.param_size[0]);
  EXPECT_EQ(static_cast<short>(p1.size() + 1), param.param_size[1]);

  gsfProcessingParameters decoded{};
  ZeroStruct(decoded);
  EXPECT_EQ(encoded, gsfDecodeProcessingParameters(&decoded, &ft, buf.data()));
  EXPECT_EQ(2, decoded.number_parameters);
  EXPECT_STREQ("FILTER=MEDIAN", decoded.param[0]);
  EXPECT_STREQ("TIDE=OBSERVED", decoded.param[1]);

  std::fill(buf.begin(), buf.begin() + 128, 0);
  gsfProcessingParameters partial{};
  ZeroStruct(partial);
  partial.param_time = {1700000301, 0};
  partial.number_parameters = 2;
  partial.param[0] = p0.data();
  partial.param[1] = nullptr;
  const int partial_bytes =
      gsfEncodeProcessingParameters(buf.data(), &partial);
  EXPECT_EQ(10 + 2 + static_cast<int>(p0.size() + 1), partial_bytes);
  EXPECT_EQ(2, ReadBe16(buf, 8));
  EXPECT_EQ(p0.size() + 1, ReadBe16(buf, 10));
}

TEST(GsfEncTest, EncodeSensorParametersRoundTripAndNullEntry) {
  auto buf = MakeEncodeBuffer();
  GSF_FILE_TABLE ft{};
  ZeroStruct(ft);
  absl::Cleanup cleanup([&ft] { ResetDecodeState(ft); });

  std::string s0 = "ROLL_OFFSET=0.12";
  std::string s1 = "PITCH_OFFSET=-0.34";
  gsfSensorParameters param{};
  ZeroStruct(param);
  param.param_time = {1700000400, 400000000};
  param.number_parameters = 2;
  param.param[0] = s0.data();
  param.param[1] = s1.data();

  const int encoded = gsfEncodeSensorParameters(buf.data(), &param);
  EXPECT_EQ(10 + 2 + static_cast<int>(s0.size() + 1) + 2 +
                static_cast<int>(s1.size() + 1),
            encoded);
  gsfSensorParameters decoded{};
  ZeroStruct(decoded);
  EXPECT_EQ(encoded, gsfDecodeSensorParameters(&decoded, &ft, buf.data()));
  EXPECT_EQ(2, decoded.number_parameters);
  EXPECT_STREQ("ROLL_OFFSET=0.12", decoded.param[0]);
  EXPECT_STREQ("PITCH_OFFSET=-0.34", decoded.param[1]);

  std::fill(buf.begin(), buf.begin() + 128, 0);
  gsfSensorParameters partial{};
  ZeroStruct(partial);
  partial.param_time = {1700000401, 0};
  partial.number_parameters = 2;
  partial.param[0] = s0.data();
  partial.param[1] = nullptr;
  const int partial_bytes = gsfEncodeSensorParameters(buf.data(), &partial);
  EXPECT_EQ(10 + 2 + static_cast<int>(s0.size() + 1), partial_bytes);
  EXPECT_EQ(2, ReadBe16(buf, 8));
  EXPECT_EQ(s0.size() + 1, ReadBe16(buf, 10));
}

TEST(GsfEncTest, EncodeCommentRoundTripAndEmptyComment) {
  auto buf = MakeEncodeBuffer();
  GSF_FILE_TABLE ft{};
  ZeroStruct(ft);
  absl::Cleanup cleanup([&ft] { ResetDecodeState(ft); });

  // 1. Non-empty comment round-trip.
  std::string text = "Survey line 01 test comment";
  gsfComment comment{};
  ZeroStruct(comment);
  comment.comment_time = {1700000500, 500000000};
  comment.comment_length = static_cast<int>(text.size());
  comment.comment = text.data();

  const int encoded = gsfEncodeComment(buf.data(), &comment);
  EXPECT_EQ(12 + comment.comment_length, encoded);
  gsfComment decoded{};
  ZeroStruct(decoded);
  EXPECT_EQ(encoded, gsfDecodeComment(&decoded, &ft, buf.data()));
  EXPECT_EQ(comment.comment_length, decoded.comment_length);
  EXPECT_EQ(text, std::string_view(decoded.comment, decoded.comment_length));

  // 2. Empty comment overwrites pre-filled 0xA5 buffer with zero length.
  std::fill(buf.begin(), buf.begin() + 64, 0xA5);
  char empty[] = "";
  gsfComment empty_comment{};
  ZeroStruct(empty_comment);
  empty_comment.comment_time = {1700000501, 0};
  empty_comment.comment_length = 0;
  empty_comment.comment = empty;
  EXPECT_EQ(12, gsfEncodeComment(buf.data(), &empty_comment));
  EXPECT_EQ(0u, ReadBe32(buf, 8));
}

TEST(GsfEncTest, EncodeHistoryRoundTripAndNullPointers) {
  auto buf = MakeEncodeBuffer();
  GSF_FILE_TABLE ft{};
  ZeroStruct(ft);
  absl::Cleanup cleanup([&ft] { ResetDecodeState(ft); });

  // 1. History with non-null command line and comment (8 + 12+10 + 18+18 + 8 = 73).
  std::string cmd = "mbprocess --clean";
  std::string note = "Applied tide table";
  gsfHistory hist{};
  ZeroStruct(hist);
  hist.history_time = {1700000600, 600000000};
  CopyStringToFixedBuffer("vessel-host", hist.host_name);
  CopyStringToFixedBuffer("operator", hist.operator_name);
  hist.command_line = cmd.data();
  hist.comment = note.data();

  const int encoded = gsfEncodeHistory(buf.data(), &hist);
  EXPECT_EQ(73, encoded);
  gsfHistory decoded{};
  ZeroStruct(decoded);
  EXPECT_EQ(encoded, gsfDecodeHistory(&decoded, &ft, buf.data()));
  EXPECT_STREQ("vessel-host", decoded.host_name);
  EXPECT_STREQ("operator", decoded.operator_name);
  EXPECT_STREQ("mbprocess --clean", decoded.command_line);
  EXPECT_STREQ("Applied tide table", decoded.comment);

  // 2. Null command_line/comment pointers normalize to empty strings.
  std::fill(buf.begin(), buf.begin() + 128, 0);
  gsfHistory nulls{};
  ZeroStruct(nulls);
  nulls.history_time = {1700000601, 0};
  CopyStringToFixedBuffer("h1", nulls.host_name);
  CopyStringToFixedBuffer("op", nulls.operator_name);
  nulls.command_line = nullptr;
  nulls.comment = nullptr;
  const int null_bytes = gsfEncodeHistory(buf.data(), &nulls);
  EXPECT_EQ(8 + 2 + 3 + 2 + 3 + 2 + 1 + 2 + 0, null_bytes);
  EXPECT_STREQ("", nulls.command_line);
  EXPECT_STREQ("", nulls.comment);
}

TEST(GsfEncTest, EncodeNavigationErrorRoundTrip) {
  auto buf = MakeEncodeBuffer();
  gsfNavigationError err{};
  ZeroStruct(err);
  err.nav_error_time = {1700000700, 700000000};
  err.record_id = 42;
  err.longitude_error = 1.5;
  err.latitude_error = 2.3;

  const int encoded = gsfEncodeNavigationError(buf.data(), &err);
  EXPECT_EQ(20, encoded);

  gsfNavigationError decoded{};
  ZeroStruct(decoded);
  EXPECT_EQ(encoded, gsfDecodeNavigationError(&decoded, buf.data()));
  EXPECT_EQ(42, decoded.record_id);
  EXPECT_NEAR(1.5, decoded.longitude_error, 0.05);
  EXPECT_NEAR(2.3, decoded.latitude_error, 0.05);
}

TEST(GsfEncTest, EncodeHVNavigationErrorRoundTripAndNullPositionType) {
  auto buf = MakeEncodeBuffer();
  GSF_FILE_TABLE ft{};
  ZeroStruct(ft);
  absl::Cleanup cleanup([&ft] { ResetDecodeState(ft); });

  // 1. Positive and negative horizontal/vertical errors plus spare bytes.
  std::string pos = GSF_POS_TYPE_GPSU;
  for (const bool neg : {false, true}) {
    std::fill(buf.begin(), buf.begin() + 128, 0);
    gsfHVNavigationError hv{};
    ZeroStruct(hv);
    hv.nav_error_time = {1700000800, 800000000};
    hv.record_id = 88;
    hv.horizontal_error = neg ? -1.25 : 1.25;
    hv.vertical_error = neg ? -0.75 : 0.75;
    hv.SEP_uncertainty = 2.5;
    hv.spare[0] = 'A';
    hv.spare[1] = 'B';
    hv.position_type = pos.data();

    const int encoded = gsfEncodeHVNavigationError(buf.data(), &hv);
    EXPECT_EQ(26 + static_cast<int>(pos.size()), encoded);
    EXPECT_EQ('A', buf[22]);
    EXPECT_EQ('B', buf[23]);
    gsfHVNavigationError decoded{};
    ZeroStruct(decoded);
    EXPECT_EQ(encoded, gsfDecodeHVNavigationError(&decoded, &ft, buf.data()));
    EXPECT_EQ(88, decoded.record_id);
    EXPECT_NEAR(hv.horizontal_error, decoded.horizontal_error, 0.001);
    EXPECT_NEAR(hv.vertical_error, decoded.vertical_error, 0.001);
    EXPECT_NEAR(2.5, decoded.SEP_uncertainty, 0.01);
    EXPECT_STREQ(GSF_POS_TYPE_GPSU, decoded.position_type);
  }

  // 2. Null position_type overwrites pre-filled 0xA5 buffer with 0 len + NUL.
  std::fill(buf.begin(), buf.begin() + 64, 0xA5);
  gsfHVNavigationError null_pos{};
  ZeroStruct(null_pos);
  null_pos.nav_error_time = {1700000801, 0};
  null_pos.record_id = 99;
  null_pos.horizontal_error = 1.0;
  null_pos.vertical_error = 0.5;
  null_pos.SEP_uncertainty = 0.0;
  null_pos.position_type = nullptr;
  EXPECT_EQ(27, gsfEncodeHVNavigationError(buf.data(), &null_pos));
  EXPECT_EQ(0, ReadBe16(buf, 24));
  EXPECT_EQ('\0', buf[26]);
}

TEST(GsfEncTest, EncodeAttitudeRoundTripWithPositiveAndNegativeNsecDeltas) {
  auto buf = MakeEncodeBuffer();
  GSF_FILE_TABLE ft{};
  ZeroStruct(ft);
  absl::Cleanup cleanup([&ft] { ResetDecodeState(ft); });

  std::array<struct timespec, 3> times = {{
      {1700000000, 800000000},
      {1700000001, 200000000},
      {1700000002, 900000000},
  }};
  std::array<double, 3> pitch = {-1.25, 0.0, 2.35};
  std::array<double, 3> roll = {3.10, -2.20, 0.50};
  std::array<double, 3> heave = {-0.45, 0.25, -0.10};
  std::array<double, 3> heading = {10.0, 185.5, 359.25};

  gsfAttitude att{};
  ZeroStruct(att);
  att.num_measurements = 3;
  att.attitude_time = times.data();
  att.pitch = pitch.data();
  att.roll = roll.data();
  att.heave = heave.data();
  att.heading = heading.data();

  const int encoded = gsfEncodeAttitude(buf.data(), &att);
  EXPECT_EQ(40, encoded);
  gsfAttitude decoded{};
  ZeroStruct(decoded);
  EXPECT_EQ(encoded, gsfDecodeAttitude(&decoded, &ft, buf.data()));
  EXPECT_EQ(3, decoded.num_measurements);
  EXPECT_EQ(times[1].tv_sec, decoded.attitude_time[1].tv_sec);
  EXPECT_NEAR(static_cast<double>(times[1].tv_nsec),
              static_cast<double>(decoded.attitude_time[1].tv_nsec), 1.0e6);
  EXPECT_NEAR(-1.25, decoded.pitch[0], 0.01);
  EXPECT_NEAR(-2.20, decoded.roll[1], 0.01);
  EXPECT_NEAR(-0.45, decoded.heave[0], 0.01);
  EXPECT_NEAR(185.5, decoded.heading[1], 0.01);
}

TEST(GsfEncTest, SetDefaultScaleFactorCoversAllArraysAndClampsLargeValues) {
  gsfSwathBathyPing ping;
  InitBasePing(ping, 4, true);
  auto arrays = PingBuffers::Create(4, true);
  arrays.nominal_depth[0] = 1000.0;
  arrays.across_track[0] = -500.0;
  arrays.across_track[1] = 25.0;
  arrays.depth[0] = 70000.0;
  arrays.AttachAll(ping);

  EXPECT_EQ(0, gsfSetDefaultScaleFactor(&ping));
  const auto& table = ping.scaleFactors.scaleTable;
  EXPECT_DOUBLE_EQ(1.0, table[GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY - 1]
                            .multiplier);
  EXPECT_DOUBLE_EQ(
      50.0,
      table[GSF_SWATH_BATHY_SUBRECORD_NOMINAL_DEPTH_ARRAY - 1].multiplier);
  EXPECT_DOUBLE_EQ(50.0, table[GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY - 1]
                             .multiplier);
  EXPECT_GT(table[GSF_SWATH_BATHY_SUBRECORD_ALONG_TRACK_ARRAY - 1].multiplier,
            0.0);
  EXPECT_GT(table[GSF_SWATH_BATHY_SUBRECORD_TRAVEL_TIME_ARRAY - 1].multiplier,
            0.0);
  EXPECT_GT(table[GSF_SWATH_BATHY_SUBRECORD_BEAM_ANGLE_ARRAY - 1].multiplier,
            0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_MEAN_CAL_AMPLITUDE_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_MEAN_REL_AMPLITUDE_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(table[GSF_SWATH_BATHY_SUBRECORD_ECHO_WIDTH_ARRAY - 1].multiplier,
            0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_QUALITY_FACTOR_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_RECEIVE_HEAVE_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(table[GSF_SWATH_BATHY_SUBRECORD_DEPTH_ERROR_ARRAY - 1].multiplier,
            0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ERROR_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_ALONG_TRACK_ERROR_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_SIGNAL_TO_NOISE_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_BEAM_ANGLE_FORWARD_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_VERTICAL_ERROR_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_HORIZONTAL_ERROR_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_SECTOR_NUMBER_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_DETECTION_INFO_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_INCIDENT_BEAM_ADJ_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_SYSTEM_CLEANING_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_DOPPLER_CORRECTION_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(
      table[GSF_SWATH_BATHY_SUBRECORD_SONAR_VERT_UNCERT_ARRAY - 1].multiplier,
      0.0);
  EXPECT_GT(table[GSF_SWATH_BATHY_SUBRECORD_MEAN_ABS_COEF_ARRAY - 1].multiplier,
            0.0);
  EXPECT_GT(table[GSF_SWATH_BATHY_SUBRECORD_TVG_ARRAY - 1].multiplier, 0.0);
}

const SubrecordSlice* FindSubrecord(std::span<const SubrecordSlice> slices,
                                    unsigned int id) {
  for (const auto& slice : slices) {
    if (slice.id == static_cast<uint8_t>(id)) return &slice;
  }
  return nullptr;
}

void PopulateSensorImagery(gsfSensorImagery& img, unsigned int sensor_id,
                           bool negative_em4) {
  switch (sensor_id) {
    case GSF_SWATH_BATHY_SUBRECORD_EM2000_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM1002_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM300_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM120_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3002_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3000D_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3002D_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM121A_SIS_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM2000_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3000_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM1002_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM300_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM120_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3002_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3000D_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3002D_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM121A_SIS_RAW_SPECIFIC: {
      auto& em3 = img.gsfEM3ImagerySpecific;
      em3.range_norm = 120;
      em3.start_tvg_ramp = 10;
      em3.stop_tvg_ramp = 50;
      em3.bsn = 20;
      em3.bso = 25;
      em3.mean_absorption = 12.5;
      em3.offset = 60;
      em3.scale = 2;
      break;
    }
    case GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC:
      img.gsfReson7100ImagerySpecific.size = 64;
      break;
    case GSF_SWATH_BATHY_SUBRECORD_RESON_TSERIES_SPECIFIC:
      img.gsfResonTSeriesImagerySpecific.size = 64;
      break;
    case GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_RESON_8111_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_RESON_8124_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_RESON_8125_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_RESON_8150_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_RESON_8160_SPECIFIC:
      img.gsfReson8100ImagerySpecific.spare[0] = 0x5A;
      break;
    case GSF_SWATH_BATHY_SUBRECORD_EM122_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM302_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM710_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM2040_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_ME70BO_SPECIFIC: {
      auto& em4 = img.gsfEM4ImagerySpecific;
      em4.sampling_frequency = 25000.5;
      em4.mean_absorption = 14.25;
      em4.tx_pulse_length = 250.0;
      em4.range_norm = 150;
      em4.start_tvg_ramp = 12;
      em4.stop_tvg_ramp = 64;
      em4.bsn = negative_em4 ? -18.5 : 18.5;
      em4.bso = negative_em4 ? -24.5 : 24.5;
      em4.tx_beam_width = 1.5;
      em4.tvg_cross_over = 20.0;
      em4.offset = 100;
      em4.scale = 10;
      break;
    }
    case GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC:
      img.gsfKMALLImagerySpecific.spare[0] = 0x11;
      break;
    case GSF_SWATH_BATHY_SUBRECORD_KLEIN_5410_BSS_SPECIFIC: {
      auto& klein = img.gsfKlein5410BssImagerySpecific;
      klein.res_mode = 1;
      klein.tvg_page = 3;
      for (unsigned int i = 0; i < 5; ++i) {
        klein.beam_id[i] = i + 1;
      }
      break;
    }
    case GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2020_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2022_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2024_SPECIFIC: {
      auto& r2i = img.gsfR2SonicImagerySpecific;
      CopyBytesToFixedBuffer("2022", r2i.model_number);
      CopyBytesToFixedBuffer("100042", r2i.serial_number);
      r2i.dg_time = {1700000400, 100000000};
      r2i.ping_number = 909;
      r2i.ping_period = 0.08;
      r2i.sound_speed = 1503.2;
      r2i.frequency = 400000.0;
      r2i.tx_power = 215.0;
      r2i.tx_pulse_width = 0.00015;
      r2i.tx_beamwidth_vert = 1.0;
      r2i.tx_beamwidth_horiz = 0.5;
      r2i.tx_steering_vert = 1.25;
      r2i.tx_steering_horiz = 0.5;
      r2i.rx_bandwidth = 50000.0;
      r2i.rx_sample_rate = 60000.0;
      r2i.rx_range = 80.0;
      r2i.rx_gain = 14.0;
      r2i.rx_spreading = 20.0;
      r2i.rx_absorption = 105.0;
      r2i.rx_mount_tilt = 2.0;
      r2i.num_beams = 2;
      for (int i = 0; i < 6; ++i) {
        r2i.more_info[i] = 0.1 * (i + 1);
      }
      break;
    }
    default:
      break;
  }
}

void PopulateSensorDataForTest(gsfSwathBathyPing& ping,
                               unsigned int sensor_id,
                               bool negative_signed, bool alt_variant) {
  const double sign = negative_signed ? -1.0 : 1.0;
  switch (sensor_id) {
    case GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC:
      ping.sensor_data.gsfSeaBeamSpecific.EclipseTime = 123;
      break;
    case GSF_SWATH_BATHY_SUBRECORD_EM12_SPECIFIC:
      ping.sensor_data.gsfEM12Specific.ping_number = 10;
      ping.sensor_data.gsfEM12Specific.resolution = 1;
      ping.sensor_data.gsfEM12Specific.ping_quality = 40;
      ping.sensor_data.gsfEM12Specific.sound_velocity = 1500.5;
      ping.sensor_data.gsfEM12Specific.mode = 3;
      break;
    case GSF_SWATH_BATHY_SUBRECORD_EM100_SPECIFIC:
      ping.sensor_data.gsfEM100Specific.ship_pitch = sign * 1.25;
      ping.sensor_data.gsfEM100Specific.transducer_pitch = sign * 0.75;
      ping.sensor_data.gsfEM100Specific.mode = 1;
      ping.sensor_data.gsfEM100Specific.power = 2;
      ping.sensor_data.gsfEM100Specific.attenuation = 3;
      ping.sensor_data.gsfEM100Specific.tvg = 4;
      ping.sensor_data.gsfEM100Specific.pulse_length = 5;
      ping.sensor_data.gsfEM100Specific.counter = 678;
      break;
    case GSF_SWATH_BATHY_SUBRECORD_EM950_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM1000_SPECIFIC: {
      auto& s = (sensor_id == GSF_SWATH_BATHY_SUBRECORD_EM950_SPECIFIC)
                    ? ping.sensor_data.gsfEM950Specific
                    : ping.sensor_data.gsfEM1000Specific;
      s.ping_number = 234;
      s.mode = 2;
      s.ping_quality = 80;
      s.ship_pitch = sign * 1.5;
      s.transducer_pitch = sign * 0.5;
      s.surface_velocity = 1502.4;
      break;
    }
    case GSF_SWATH_BATHY_SUBRECORD_EM121A_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM121_SPECIFIC: {
      auto& s = (sensor_id == GSF_SWATH_BATHY_SUBRECORD_EM121A_SPECIFIC)
                    ? ping.sensor_data.gsfEM121ASpecific
                    : ping.sensor_data.gsfEM121Specific;
      s.ping_number = 345;
      s.mode = 1;
      s.valid_beams = 120;
      s.pulse_length = 4;
      s.beam_width = 2;
      s.tx_power = 3;
      s.tx_status = 0;
      s.rx_status = 0;
      s.surface_velocity = 1501.0;
      break;
    }
    case GSF_SWATH_BATHY_SUBRECORD_SASS_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_TYPEIII_SEABEAM_SPECIFIC: {
      auto& s = (sensor_id == GSF_SWATH_BATHY_SUBRECORD_SASS_SPECIFIC)
                    ? ping.sensor_data.gsfSASSSpecific
                    : ping.sensor_data.gsfTypeIIISeaBeamSpecific;
      s.leftmost_beam = 1;
      s.rightmost_beam = 16;
      s.total_beams = 16;
      s.nav_mode = 2;
      s.ping_number = 12;
      s.mission_number = 3;
      break;
    }
    case GSF_SWATH_BATHY_SUBRECORD_CMP_SASS_SPECIFIC:
      ping.sensor_data.gsfCmpSassSpecific.lfreq = 4920.5;
      ping.sensor_data.gsfCmpSassSpecific.lntens = 0.75;
      break;
    case GSF_SWATH_BATHY_SUBRECORD_SEAMAP_SPECIFIC: {
      auto& sm = ping.sensor_data.gsfSeamapSpecific;
      sm.portTransmitter[0] = 100.5;
      sm.portTransmitter[1] = 200.5;
      sm.stbdTransmitter[0] = 101.5;
      sm.stbdTransmitter[1] = 201.5;
      sm.portGain = 12.5;
      sm.stbdGain = 13.5;
      sm.portPulseLength = 4.2;
      sm.stbdPulseLength = 4.3;
      sm.pressureDepth = 250.5;
      sm.altitude = 45.2;
      sm.temperature = 14.8;
      break;
    }
    case GSF_SWATH_BATHY_SUBRECORD_SEABAT_SPECIFIC:
      ping.sensor_data.gsfSeaBatSpecific.ping_number = 111;
      ping.sensor_data.gsfSeaBatSpecific.surface_velocity = 1500.0;
      ping.sensor_data.gsfSeaBatSpecific.mode = GSF_SEABAT_WIDE_MODE;
      ping.sensor_data.gsfSeaBatSpecific.sonar_range = 75;
      ping.sensor_data.gsfSeaBatSpecific.transmit_power = 4;
      ping.sensor_data.gsfSeaBatSpecific.receive_gain = 10;
      break;
    case GSF_SWATH_BATHY_SUBRECORD_SB_AMP_SPECIFIC:
      ping.sensor_data.gsfSBAmpSpecific.hour = 12;
      ping.sensor_data.gsfSBAmpSpecific.minute = 34;
      ping.sensor_data.gsfSBAmpSpecific.second = 56;
      ping.sensor_data.gsfSBAmpSpecific.hundredths = 78;
      ping.sensor_data.gsfSBAmpSpecific.block_number = 9001;
      ping.sensor_data.gsfSBAmpSpecific.avg_gate_depth =
          negative_signed ? -15 : 15;
      break;
    case GSF_SWATH_BATHY_SUBRECORD_SEABAT_II_SPECIFIC:
      ping.sensor_data.gsfSeaBatIISpecific.ping_number = 222;
      ping.sensor_data.gsfSeaBatIISpecific.surface_velocity = 1500.2;
      ping.sensor_data.gsfSeaBatIISpecific.mode = GSF_SEABAT_9003;
      ping.sensor_data.gsfSeaBatIISpecific.sonar_range = 80;
      ping.sensor_data.gsfSeaBatIISpecific.transmit_power = 5;
      ping.sensor_data.gsfSeaBatIISpecific.receive_gain = 12;
      ping.sensor_data.gsfSeaBatIISpecific.fore_aft_bw = 1.5;
      ping.sensor_data.gsfSeaBatIISpecific.athwart_bw = 1.5;
      break;
    case GSF_SWATH_BATHY_SUBRECORD_SEABAT_8101_SPECIFIC: {
      auto& sb81 = ping.sensor_data.gsfSeaBat8101Specific;
      sb81.ping_number = 333;
      sb81.surface_velocity = 1501.5;
      sb81.mode = GSF_8101_AMPLITUDE;
      sb81.range = 100;
      sb81.power = 6;
      sb81.gain = 18;
      sb81.pulse_width = 120;
      sb81.tvg_spreading = 80;
      sb81.tvg_absorption = 30;
      sb81.fore_aft_bw = 1.5;
      sb81.athwart_bw = 1.5;
      sb81.range_filt_min = 2.0;
      sb81.range_filt_max = 120.0;
      sb81.depth_filt_min = 1.0;
      sb81.depth_filt_max = 100.0;
      sb81.projector = 1;
      break;
    }
    case GSF_SWATH_BATHY_SUBRECORD_SEABEAM_2112_SPECIFIC: {
      auto& sb2112 = ping.sensor_data.gsfSeaBeam2112Specific;
      sb2112.mode = GSF_2112_SVP_CORRECTION;
      sb2112.surface_velocity = 1500.0;
      sb2112.ssv_source = 'V';
      sb2112.ping_gain = 12;
      sb2112.pulse_width = 3;
      sb2112.transmitter_attenuation = 6;
      sb2112.number_algorithms = 2;
      CopyStringToFixedBuffer("W B ", sb2112.algorithm_order);
      break;
    }
    case GSF_SWATH_BATHY_SUBRECORD_ELAC_MKII_SPECIFIC:
      ping.sensor_data.gsfElacMkIISpecific.mode = GSF_MKII_SOURCE_MODE;
      ping.sensor_data.gsfElacMkIISpecific.ping_num = 55;
      ping.sensor_data.gsfElacMkIISpecific.sound_vel = 15000;
      ping.sensor_data.gsfElacMkIISpecific.pulse_length = 25;
      ping.sensor_data.gsfElacMkIISpecific.receiver_gain_stbd = 10;
      ping.sensor_data.gsfElacMkIISpecific.receiver_gain_port = 11;
      ping.sensor_data.gsfElacMkIISpecific.reserved = 0;
      break;
    case GSF_SWATH_BATHY_SUBRECORD_EM2000_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM1002_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM300_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM120_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3002_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3000D_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3002D_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM121A_SIS_SPECIFIC:
      PopulateEM3Specific(ping.sensor_data.gsfEM3Specific);
      break;
    case GSF_SWATH_BATHY_SUBRECORD_EM2000_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3000_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM1002_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM300_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM120_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3002_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3000D_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM3002D_RAW_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM121A_SIS_RAW_SPECIFIC: {
      const int model = alt_variant ? 1002
                                    : (negative_signed ? 300 : 2000);
      PopulateEM3RawSpecific(ping.sensor_data.gsfEM3RawSpecific, model,
                             negative_signed);
      break;
    }
    case GSF_SWATH_BATHY_SUBRECORD_EM710_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM302_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM122_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_EM2040_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_ME70BO_SPECIFIC:
      PopulateEM4Specific(ping.sensor_data.gsfEM4Specific, 710,
                          negative_signed);
      break;
    case GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC:
      PopulateKMALLSpecific(ping.sensor_data.gsfKMALLSpecific, negative_signed,
                            alt_variant);
      break;
    case GSF_SWATH_BATHY_SUBRECORD_GEOSWATH_PLUS_SPECIFIC:
      PopulateGeoSwathPlusSpecific(ping.sensor_data.gsfGeoSwathPlusSpecific);
      break;
    case GSF_SWATH_BATHY_SUBRECORD_KLEIN_5410_BSS_SPECIFIC:
      PopulateKlein5410BssSpecific(ping.sensor_data.gsfKlein5410BssSpecific);
      break;
    case GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_RESON_8111_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_RESON_8124_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_RESON_8125_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_RESON_8150_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_RESON_8160_SPECIFIC:
      PopulateReson8100Specific(ping.sensor_data.gsfReson8100Specific, 8101);
      break;
    case GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC:
      PopulateReson7100Specific(ping.sensor_data.gsfReson7100Specific);
      break;
    case GSF_SWATH_BATHY_SUBRECORD_RESON_TSERIES_SPECIFIC:
      PopulateResonTSeriesSpecific(ping.sensor_data.gsfResonTSeriesSpecific,
                                   negative_signed);
      break;
    case GSF_SWATH_BATHY_SUBRECORD_DELTA_T_SPECIFIC:
      PopulateDeltaTSpecific(ping.sensor_data.gsfDeltaTSpecific);
      break;
    case GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2020_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2022_SPECIFIC:
    case GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2024_SPECIFIC:
      PopulateR2SonicSpecific(ping.sensor_data.gsfR2SonicSpecific);
      break;
    case GSF_SWATH_BATHY_SB_SUBRECORD_ECHOTRAC_SPECIFIC:
      ping.sensor_data.gsfSBEchotracSpecific.navigation_error = 5;
      ping.sensor_data.gsfSBEchotracSpecific.mpp_source =
          GSF_SB_MPP_SOURCE_DGPS_TRIMBLE;
      ping.sensor_data.gsfSBEchotracSpecific.tide_source = 1;
      ping.sensor_data.gsfSBEchotracSpecific.dynamic_draft = sign * 0.42;
      break;
    case GSF_SWATH_BATHY_SB_SUBRECORD_BATHY2000_SPECIFIC:
      ping.sensor_data.gsfSBBathy2000Specific.navigation_error = 4;
      ping.sensor_data.gsfSBBathy2000Specific.mpp_source =
          GSF_SB_MPP_SOURCE_GPS_3S;
      ping.sensor_data.gsfSBBathy2000Specific.tide_source = 2;
      ping.sensor_data.gsfSBBathy2000Specific.dynamic_draft = sign * 0.31;
      break;
    case GSF_SWATH_BATHY_SB_SUBRECORD_PDD_SPECIFIC:
      ping.sensor_data.gsfSBPDDSpecific.navigation_error = 3;
      ping.sensor_data.gsfSBPDDSpecific.mpp_source =
          GSF_SB_MPP_SOURCE_RANGE_MFIX;
      ping.sensor_data.gsfSBPDDSpecific.tide_source = 1;
      ping.sensor_data.gsfSBPDDSpecific.dynamic_draft = sign * 0.18;
      break;
    case GSF_SWATH_BATHY_SB_SUBRECORD_MGD77_SPECIFIC:
      ping.sensor_data.gsfSBMGD77Specific.time_zone_corr = 6;
      ping.sensor_data.gsfSBMGD77Specific.position_type_code = 1;
      ping.sensor_data.gsfSBMGD77Specific.correction_code = 2;
      ping.sensor_data.gsfSBMGD77Specific.bathy_type_code = 3;
      ping.sensor_data.gsfSBMGD77Specific.quality_code = 4;
      ping.sensor_data.gsfSBMGD77Specific.travel_time = 0.0432;
      break;
    case GSF_SWATH_BATHY_SB_SUBRECORD_BDB_SPECIFIC:
      ping.sensor_data.gsfSBBDBSpecific.doc_no = 54321;
      ping.sensor_data.gsfSBBDBSpecific.eval = '2';
      ping.sensor_data.gsfSBBDBSpecific.classification = 'U';
      ping.sensor_data.gsfSBBDBSpecific.track_adj_flag = 'N';
      ping.sensor_data.gsfSBBDBSpecific.source_flag = 'R';
      ping.sensor_data.gsfSBBDBSpecific.pt_or_track_ln = 'D';
      ping.sensor_data.gsfSBBDBSpecific.datum_flag = 'W';
      break;
    case GSF_SWATH_BATHY_SB_SUBRECORD_NOSHDB_SPECIFIC:
      ping.sensor_data.gsfSBNOSHDBSpecific.type_code = 3;
      ping.sensor_data.gsfSBNOSHDBSpecific.carto_code = 9;
      break;
    case GSF_SWATH_BATHY_SB_SUBRECORD_NAVISOUND_SPECIFIC:
      ping.sensor_data.gsfSBNavisoundSpecific.pulse_length = 0.55;
      break;
    default:
      break;
  }
}

TEST(GsfEncTest, EncodeSwathBathymetryPingRejectsInvalidAndMissingScales) {
  auto buf = MakeEncodeBuffer();
  // 1. Non-positive number_beams fails with GSF_INVALID_NUM_BEAMS.
  {
    GSF_FILE_TABLE ft{};
    ZeroStruct(ft);
    gsfSwathBathyPing bad;
    InitBasePing(bad, 0, false);
    gsfError = GSF_NORMAL;
    EXPECT_EQ(-1, gsfEncodeSwathBathymetryPing(buf.data(), &bad, &ft));
    EXPECT_EQ(GSF_INVALID_NUM_BEAMS, gsfIntError());

    bad.number_beams = -2;
    gsfError = GSF_NORMAL;
    EXPECT_EQ(-1, gsfEncodeSwathBathymetryPing(buf.data(), &bad, &ft));
    EXPECT_EQ(GSF_INVALID_NUM_BEAMS, gsfIntError());
  }
  // 2. Unrecognized sensor_id fails with GSF_UNRECOGNIZED_SENSOR_ID.
  {
    GSF_FILE_TABLE ft{};
    ZeroStruct(ft);
    gsfSwathBathyPing bad_sensor;
    InitBasePing(bad_sensor, 4, false);
    bad_sensor.sensor_id = 999;
    gsfError = GSF_NORMAL;
    EXPECT_EQ(-1, gsfEncodeSwathBathymetryPing(buf.data(), &bad_sensor, &ft));
    EXPECT_EQ(GSF_UNRECOGNIZED_SENSOR_ID, gsfIntError());
  }
  // 3. Previously loaded scales with zero current ping multipliers fail cleanly.
  {
    GSF_FILE_TABLE ft{};
    ZeroStruct(ft);
    ft.scales_read = 1;
    gsfSwathBathyPing bad_sf;
    InitBasePing(bad_sf, 4, false);
    bad_sf.scaleFactors.numArraySubrecords = 1;
    gsfError = GSF_NORMAL;
    EXPECT_EQ(-1, gsfEncodeSwathBathymetryPing(buf.data(), &bad_sf, &ft));
    EXPECT_EQ(GSF_ILLEGAL_SCALE_FACTOR_MULTIPLIER, gsfIntError());
  }

  auto arrays = PingBuffers::Create(4, false);
  auto expect_missing_scale = [&](unsigned int id, unsigned char flag,
                                  auto attach_fn) {
    GSF_FILE_TABLE ft{};
    ZeroStruct(ft);
    ft.major_version_number = 3;
    gsfSwathBathyPing ping;
    InitBasePing(ping, 4, false);
    ping.scaleFactors.scaleTable[id - 1].compressionFlag =
        static_cast<unsigned char>(flag);
    ft.rec.mb_ping.scaleFactors.scaleTable[id - 1].compressionFlag =
        static_cast<unsigned char>(flag);
    attach_fn(ping);
    gsfError = GSF_NORMAL;
    EXPECT_EQ(-1, gsfEncodeSwathBathymetryPing(buf.data(), &ping, &ft))
        << "subrecord id=" << id;
    EXPECT_EQ(GSF_ILLEGAL_SCALE_FACTOR_MULTIPLIER, gsfIntError())
        << "subrecord id=" << id;
  };

  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.depth = arrays.depth.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                       GSF_FIELD_SIZE_FOUR,
                       [&](gsfSwathBathyPing& p) {
                         p.depth = arrays.depth.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_NOMINAL_DEPTH_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.nominal_depth = arrays.nominal_depth.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.across_track = arrays.across_track.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY,
                       GSF_FIELD_SIZE_FOUR,
                       [&](gsfSwathBathyPing& p) {
                         p.across_track = arrays.across_track.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_ALONG_TRACK_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.along_track = arrays.along_track.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_TRAVEL_TIME_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.travel_time = arrays.travel_time.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_BEAM_ANGLE_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.beam_angle = arrays.beam_angle.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_MEAN_CAL_AMPLITUDE_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.mc_amplitude = arrays.mc_amplitude.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_MEAN_CAL_AMPLITUDE_ARRAY,
                       GSF_FIELD_SIZE_TWO,
                       [&](gsfSwathBathyPing& p) {
                         p.mc_amplitude = arrays.mc_amplitude.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_MEAN_REL_AMPLITUDE_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.mr_amplitude = arrays.mr_amplitude.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_MEAN_REL_AMPLITUDE_ARRAY,
                       GSF_FIELD_SIZE_TWO,
                       [&](gsfSwathBathyPing& p) {
                         p.mr_amplitude = arrays.mr_amplitude.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_ECHO_WIDTH_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.echo_width = arrays.echo_width.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_QUALITY_FACTOR_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.quality_factor = arrays.quality_factor.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_RECEIVE_HEAVE_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.receive_heave = arrays.receive_heave.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_DEPTH_ERROR_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.depth_error = arrays.depth_error.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ERROR_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.across_track_error =
                             arrays.across_track_error.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_ALONG_TRACK_ERROR_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.along_track_error = arrays.along_track_error.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_SIGNAL_TO_NOISE_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.signal_to_noise = arrays.signal_to_noise.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_BEAM_ANGLE_FORWARD_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.beam_angle_forward =
                             arrays.beam_angle_forward.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_VERTICAL_ERROR_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.vertical_error = arrays.vertical_error.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_HORIZONTAL_ERROR_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.horizontal_error = arrays.horizontal_error.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_SECTOR_NUMBER_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.sector_number = arrays.sector_number.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_DETECTION_INFO_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.detection_info = arrays.detection_info.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_INCIDENT_BEAM_ADJ_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.incident_beam_adj = arrays.incident_beam_adj.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_SYSTEM_CLEANING_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.system_cleaning = arrays.system_cleaning.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_DOPPLER_CORRECTION_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.doppler_corr = arrays.doppler_corr.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_SONAR_VERT_UNCERT_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.sonar_vert_uncert = arrays.sonar_vert_uncert.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_SONAR_HORZ_UNCERT_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.sonar_horz_uncert = arrays.sonar_horz_uncert.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_DETECTION_WINDOW_ARRAY,
                       GSF_FIELD_SIZE_ONE,
                       [&](gsfSwathBathyPing& p) {
                         p.detection_window = arrays.detection_window.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_MEAN_ABS_COEF_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.mean_abs_coeff = arrays.mean_abs_coeff.data();
                       });
  expect_missing_scale(GSF_SWATH_BATHY_SUBRECORD_TVG_ARRAY,
                       GSF_FIELD_SIZE_DEFAULT,
                       [&](gsfSwathBathyPing& p) {
                         p.TVG_dB = arrays.tvg_db.data();
                       });
}

TEST(GsfEncTest, EncodeSwathBathymetryPingScaleSkippingAndVersions) {
  auto buf = MakeEncodeBuffer();
  auto arrays = PingBuffers::Create(4, true);

  GSF_FILE_TABLE ft{};
  ZeroStruct(ft);
  ft.major_version_number = 3;
  ft.minor_version_number = 11;
  ft.scales_read = 1;

  gsfSwathBathyPing ping;
  InitBasePing(ping, 4, true);
  ping.depth = arrays.depth.data();
  LoadScale(ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
            GSF_FIELD_SIZE_DEFAULT, 0.01, 0);

  // 1. First v3 encode emits SCALE_FACTORS alongside DEPTH_ARRAY.
  const int bytes1 = gsfEncodeSwathBathymetryPing(buf.data(), &ping, &ft);
  ASSERT_GT(bytes1, 56);
  EXPECT_EQ(0, ft.scales_read);
  // Tide corrector is encoded at byte offset 24 as signed centi-meters.
  EXPECT_EQ(-125, ReadBeS16(buf, 24));
  auto slices1 = ParsePingSubrecords(
      std::span<const unsigned char>(buf.data(), bytes1), 56);
  EXPECT_NE(nullptr, FindSubrecord(slices1,
                                   GSF_SWATH_BATHY_SUBRECORD_SCALE_FACTORS));
  EXPECT_NE(nullptr, FindSubrecord(slices1,
                                   GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY));
  EXPECT_NE(nullptr, FindSubrecord(slices1,
                                   GSF_SWATH_BATHY_SUBRECORD_UNKNOWN));

  // 2. Second encode with identical scale factors omits SCALE_FACTORS.
  std::fill(buf.begin(), buf.begin() + 256, 0);
  const int bytes2 = gsfEncodeSwathBathymetryPing(buf.data(), &ping, &ft);
  ASSERT_GT(bytes2, 56);
  auto slices2 = ParsePingSubrecords(
      std::span<const unsigned char>(buf.data(), bytes2), 56);
  EXPECT_EQ(nullptr, FindSubrecord(slices2,
                                   GSF_SWATH_BATHY_SUBRECORD_SCALE_FACTORS));

  // 3. Modified depth multiplier re-emits SCALE_FACTORS.
  LoadScale(ping.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
            GSF_FIELD_SIZE_DEFAULT, 0.02, 0);
  std::fill(buf.begin(), buf.begin() + 256, 0);
  const int bytes3 = gsfEncodeSwathBathymetryPing(buf.data(), &ping, &ft);
  ASSERT_GT(bytes3, 56);
  auto slices3 = ParsePingSubrecords(
      std::span<const unsigned char>(buf.data(), bytes3), 56);
  EXPECT_NE(nullptr, FindSubrecord(slices3,
                                   GSF_SWATH_BATHY_SUBRECORD_SCALE_FACTORS));

  // 4. Legacy v2 header framing (42 bytes) writes depth subrecord directly.
  GSF_FILE_TABLE ft_v2{};
  ZeroStruct(ft_v2);
  ft_v2.major_version_number = 2;
  ft_v2.minor_version_number = 6;
  ft_v2.scales_read = 1;
  gsfSwathBathyPing ping_pos;
  InitBasePing(ping_pos, 4, false);
  ping_pos.depth = arrays.depth.data();
  LoadScale(ping_pos.scaleFactors, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
            GSF_FIELD_SIZE_DEFAULT, 0.01, 0);
  std::fill(buf.begin(), buf.begin() + 256, 0);
  const int bytes_v2 = gsfEncodeSwathBathymetryPing(buf.data(), &ping_pos,
                                                   &ft_v2);
  ASSERT_GT(bytes_v2, 42);
  auto slices_v2 = ParsePingSubrecords(
      std::span<const unsigned char>(buf.data(), bytes_v2), 42);
  EXPECT_NE(nullptr, FindSubrecord(slices_v2,
                                   GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY));
}

TEST(GsfEncTest, EncodeSwathBathymetryPingAllArraysAndCompression) {
  auto buf = MakeEncodeBuffer();
  struct ArrayPass {
    short num_beams;
    bool negative_signed;
    unsigned char depth_size;
    unsigned char amp_size;
    unsigned char kmall_size;
    unsigned char compress_flag;
    int mc_offset = 0;
  };
  constexpr auto kArrayPasses = std::to_array<ArrayPass>({
      {.num_beams = 6,
       .negative_signed = true,
       .depth_size = GSF_FIELD_SIZE_ONE,
       .amp_size = GSF_FIELD_SIZE_ONE,
       .kmall_size = GSF_FIELD_SIZE_ONE,
       .compress_flag = GSF_DISABLE_COMPRESSION,
       .mc_offset = 50},
      {.num_beams = 4,
       .negative_signed = true,
       .depth_size = GSF_FIELD_SIZE_TWO,
       .amp_size = GSF_FIELD_SIZE_TWO,
       .kmall_size = GSF_FIELD_SIZE_TWO,
       .compress_flag = GSF_DISABLE_COMPRESSION},
      {.num_beams = 4,
       .negative_signed = false,
       .depth_size = GSF_FIELD_SIZE_FOUR,
       .amp_size = GSF_FIELD_SIZE_TWO,
       .kmall_size = GSF_FIELD_SIZE_FOUR,
       .compress_flag = GSF_DISABLE_COMPRESSION},
      {.num_beams = 8,
       .negative_signed = true,
       .depth_size = GSF_FIELD_SIZE_DEFAULT,
       .amp_size = GSF_FIELD_SIZE_DEFAULT,
       .kmall_size = GSF_FIELD_SIZE_DEFAULT,
       .compress_flag = GSF_ENABLE_COMPRESSION},
  });

  for (const auto& pass : kArrayPasses) {
    std::fill(buf.begin(), buf.begin() + 8192, 0);
    auto arrays = PingBuffers::Create(pass.num_beams, pass.negative_signed);
    gsfSwathBathyPing ping;
    InitBasePing(ping, pass.num_beams, pass.negative_signed);
    arrays.AttachAll(ping);
    LoadAllPingScales(ping.scaleFactors, pass.depth_size, pass.amp_size,
                      pass.kmall_size, pass.compress_flag, pass.mc_offset);

    GSF_FILE_TABLE ft{};
    ZeroStruct(ft);
    ft.major_version_number = 3;
    ft.minor_version_number = 11;
    ft.scales_read = 1;

    const int encoded = gsfEncodeSwathBathymetryPing(buf.data(), &ping, &ft);
    ASSERT_GT(encoded, 56);
    auto slices = ParsePingSubrecords(
        std::span<const unsigned char>(buf.data(), encoded), 56);
    EXPECT_EQ(32u, slices.size());

    if (pass.compress_flag == GSF_DISABLE_COMPRESSION) {
      const SubrecordSlice* depth_sr =
          FindSubrecord(slices, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY);
      ASSERT_NE(nullptr, depth_sr);
      // DEPTH_ARRAY supports 2- or 4-byte fields; ONE falls back to default 2.
      const uint32_t depth_width =
          pass.depth_size == GSF_FIELD_SIZE_FOUR ? 4u : 2u;
      EXPECT_EQ(static_cast<uint32_t>(pass.num_beams) * depth_width,
                depth_sr->size);

      const SubrecordSlice* across_sr =
          FindSubrecord(slices, GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY);
      ASSERT_NE(nullptr, across_sr);
      EXPECT_EQ(static_cast<uint32_t>(pass.num_beams) * depth_width,
                across_sr->size);
      if (depth_width == 2u && pass.negative_signed) {
        EXPECT_EQ(-1050, ReadBeS16(across_sr->payload, 0));
      } else if (depth_width == 4u) {
        EXPECT_EQ(1050, ReadBeS32(across_sr->payload, 0));
      }

      const SubrecordSlice* amp_sr = FindSubrecord(
          slices, GSF_SWATH_BATHY_SUBRECORD_MEAN_CAL_AMPLITUDE_ARRAY);
      ASSERT_NE(nullptr, amp_sr);
      const uint32_t amp_width = pass.amp_size == GSF_FIELD_SIZE_TWO ? 2u : 1u;
      EXPECT_EQ(static_cast<uint32_t>(pass.num_beams) * amp_width,
                amp_sr->size);

      const SubrecordSlice* det_sr = FindSubrecord(
          slices, GSF_SWATH_BATHY_SUBRECORD_DETECTION_WINDOW_ARRAY);
      ASSERT_NE(nullptr, det_sr);
      const uint32_t det_width =
          pass.kmall_size == GSF_FIELD_SIZE_ONE
              ? 1u
              : (pass.kmall_size == GSF_FIELD_SIZE_TWO ? 2u : 4u);
      EXPECT_EQ(static_cast<uint32_t>(pass.num_beams) * det_width,
                det_sr->size);
    }

    if (pass.compress_flag == GSF_ENABLE_COMPRESSION) {
      auto verify_double_compressed = [&](unsigned int id,
                                          const std::vector<double>& expected,
                                          double tol) {
        const SubrecordSlice* s = FindSubrecord(slices, id);
        ASSERT_NE(nullptr, s) << "id=" << id;
        double* decoded = nullptr;
        absl::Cleanup free_arr([&decoded, id] {
          std::free(decoded);
          arraySize[kValidDecodeHandle - 1][id - 1] = 0;
        });
        arraySize[kValidDecodeHandle - 1][id - 1] = 0;
        EXPECT_EQ(static_cast<int>(s->size),
                  DecodeCompressedArray(&decoded, s->payload.data(),
                                        pass.num_beams,
                                        static_cast<int>(s->size),
                                        &ping.scaleFactors,
                                        static_cast<int>(id),
                                        kValidDecodeHandle));
        ASSERT_NE(nullptr, decoded);
        for (int i = 0; i < pass.num_beams; ++i) {
          EXPECT_NEAR(expected[static_cast<size_t>(i)], decoded[i], tol);
        }
      };

      auto verify_ushort_compressed =
          [&](unsigned int id, const std::vector<unsigned short>& expected) {
            const SubrecordSlice* s = FindSubrecord(slices, id);
            ASSERT_NE(nullptr, s) << "id=" << id;
            unsigned short* decoded = nullptr;
            absl::Cleanup free_arr([&decoded, id] {
              std::free(decoded);
              arraySize[kValidDecodeHandle - 1][id - 1] = 0;
            });
            arraySize[kValidDecodeHandle - 1][id - 1] = 0;
            EXPECT_EQ(static_cast<int>(s->size),
                      DecodeCompressedUnsignedShortArray(
                          &decoded, s->payload.data(), pass.num_beams,
                          static_cast<int>(s->size), static_cast<int>(id),
                          kValidDecodeHandle));
            ASSERT_NE(nullptr, decoded);
            for (int i = 0; i < pass.num_beams; ++i) {
              EXPECT_EQ(expected[static_cast<size_t>(i)], decoded[i]);
            }
          };

      verify_double_compressed(GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                               arrays.depth, 0.02);
      verify_double_compressed(GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY,
                               arrays.across_track, 0.02);
      verify_double_compressed(
          GSF_SWATH_BATHY_SUBRECORD_MEAN_CAL_AMPLITUDE_ARRAY,
          arrays.mc_amplitude, 1.0);
      verify_double_compressed(GSF_SWATH_BATHY_SUBRECORD_TVG_ARRAY,
                               arrays.tvg_db, 0.2);
      verify_ushort_compressed(GSF_SWATH_BATHY_SUBRECORD_SECTOR_NUMBER_ARRAY,
                               arrays.sector_number);
      verify_ushort_compressed(GSF_SWATH_BATHY_SUBRECORD_DETECTION_INFO_ARRAY,
                               arrays.detection_info);
      verify_ushort_compressed(GSF_SWATH_BATHY_SUBRECORD_SYSTEM_CLEANING_ARRAY,
                               arrays.system_cleaning);
    }
  }
}

TEST(GsfEncTest, EncodeBRBIntensityBitDepthsSensorsAndErrors) {
  auto buf = MakeEncodeBuffer();
  // 1. 12-bit packed intensity sample layout.
  {
    std::vector<unsigned int> beam0 = {0xABC, 0x123, 0x456};
    std::vector<unsigned int> beam1 = {0x789, 0x0FE};
    std::array<gsfTimeSeriesIntensity, 2> series{};
    std::memset(series.data(), 0, series.size() * sizeof(series[0]));
    series[0].sample_count = static_cast<unsigned short>(beam0.size());
    series[0].detect_sample = 1;
    series[0].start_range_samples = 2;
    series[0].samples = beam0.data();
    series[1].sample_count = static_cast<unsigned short>(beam1.size());
    series[1].detect_sample = 1;
    series[1].start_range_samples = 4;
    series[1].samples = beam1.data();

    gsfBRBIntensity brb{};
    ZeroStruct(brb);
    brb.bits_per_sample = 12;
    brb.applied_corrections =
        GSF_INTENSITY_LINEAR | GSF_INTENSITY_CALIBRATED;
    PopulateSensorImagery(brb.sensor_imagery,
                          GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC, false);
    brb.time_series = series.data();

    GSF_FILE_TABLE ft{};
    ZeroStruct(ft);
    ft.major_version_number = 3;
    gsfSwathBathyPing ping;
    InitBasePing(ping, 2, false);
    ping.sensor_id =
        static_cast<int>(GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC);
    PopulateSensorDataForTest(
        ping, GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC, false, false);
    ping.brb_inten = &brb;

    const int encoded = gsfEncodeSwathBathymetryPing(buf.data(), &ping, &ft);
    ASSERT_GT(encoded, 56);
    auto slices = ParsePingSubrecords(
        std::span<const unsigned char>(buf.data(), encoded), 56);
    const SubrecordSlice* inten = FindSubrecord(
        slices, GSF_SWATH_BATHY_SUBRECORD_INTENSITY_SERIES_ARRAY);
    ASSERT_NE(nullptr, inten);
    // 21 fixed + 18 EM3Imagery + (12 + 6) beam0 + (12 + 3) beam1 = 72.
    EXPECT_EQ(72u, inten->size);
    EXPECT_EQ(12, inten->payload[0]);
    EXPECT_THAT(inten->payload.subspan(51, 6),
                ElementsAre(0xAB, 0xC1, 0x23, 0x45, 0x60, 0x00));
    EXPECT_THAT(inten->payload.subspan(69, 3),
                ElementsAre(0x78, 0x90, 0xFE));
  }

  // 2. Unpacked 8-, 16-, and 32-bit intensity sample layouts.
  for (const unsigned char bits : {8, 16, 32}) {
    std::vector<unsigned int> s0 = {10, 20, 30, 40};
    std::array<gsfTimeSeriesIntensity, 2> series{};
    std::memset(series.data(), 0, series.size() * sizeof(series[0]));
    for (auto& item : series) {
      item.sample_count = static_cast<unsigned short>(s0.size());
      item.samples = s0.data();
    }
    gsfBRBIntensity brb{};
    ZeroStruct(brb);
    brb.bits_per_sample = bits;
    PopulateSensorImagery(brb.sensor_imagery,
                          GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC, false);
    brb.time_series = series.data();

    GSF_FILE_TABLE ft{};
    ZeroStruct(ft);
    ft.major_version_number = 3;
    gsfSwathBathyPing ping;
    InitBasePing(ping, 2, false);
    ping.sensor_id =
        static_cast<int>(GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC);
    PopulateSensorDataForTest(
        ping, GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC, false, false);
    ping.brb_inten = &brb;

    std::fill(buf.begin(), buf.begin() + 512, 0);
    const int encoded = gsfEncodeSwathBathymetryPing(buf.data(), &ping, &ft);
    ASSERT_GT(encoded, 56);
    auto slices = ParsePingSubrecords(
        std::span<const unsigned char>(buf.data(), encoded), 56);
    const SubrecordSlice* inten = FindSubrecord(
        slices, GSF_SWATH_BATHY_SUBRECORD_INTENSITY_SERIES_ARRAY);
    ASSERT_NE(nullptr, inten);
    EXPECT_EQ(21u + 18u + 2u * (12u + 4u * (bits / 8u)), inten->size);
  }

  // 3. Sensor-specific imagery headers with exact per-sensor sizes.
  struct ImagerySensorCase {
    unsigned int sensor_id;
    uint32_t expected_imagery_size;
    bool negative_em4 = false;
  };
  constexpr auto kImageryCases = std::to_array<ImagerySensorCase>({
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC,
       .expected_imagery_size = 18},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM300_RAW_SPECIFIC,
       .expected_imagery_size = 18},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC,
       .expected_imagery_size = 66},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_TSERIES_SPECIFIC,
       .expected_imagery_size = 66},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
       .expected_imagery_size = 8},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM710_SPECIFIC,
       .expected_imagery_size = 50},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM710_SPECIFIC,
       .expected_imagery_size = 50,
       .negative_em4 = true},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC,
       .expected_imagery_size = 64},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_KLEIN_5410_BSS_SPECIFIC,
       .expected_imagery_size = 18},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2022_SPECIFIC,
       .expected_imagery_size = 168},
  });
  for (const auto& ic : kImageryCases) {
    SCOPED_TRACE(::testing::Message()
                 << "sensor_id=" << ic.sensor_id
                 << " negative_em4=" << ic.negative_em4);
    std::vector<unsigned int> samples = {1, 2};
    std::array<gsfTimeSeriesIntensity, 2> series{};
    std::memset(series.data(), 0, series.size() * sizeof(series[0]));
    series[0].sample_count = 2;
    series[0].samples = samples.data();
    series[1].sample_count = 2;
    series[1].samples = samples.data();

    gsfBRBIntensity brb{};
    ZeroStruct(brb);
    brb.bits_per_sample = 16;
    PopulateSensorImagery(brb.sensor_imagery, ic.sensor_id, ic.negative_em4);
    brb.time_series = series.data();

    GSF_FILE_TABLE ft{};
    ZeroStruct(ft);
    ft.major_version_number = 3;
    gsfSwathBathyPing ping;
    InitBasePing(ping, 2, false);
    ping.sensor_id = static_cast<int>(ic.sensor_id);
    PopulateSensorDataForTest(ping, ic.sensor_id, false, false);
    ping.brb_inten = &brb;

    std::fill(buf.begin(), buf.begin() + 2048, 0);
    const int encoded = gsfEncodeSwathBathymetryPing(buf.data(), &ping, &ft);
    ASSERT_GT(encoded, 56) << "sensor_id=" << ic.sensor_id;
    auto slices = ParsePingSubrecords(
        std::span<const unsigned char>(buf.data(), encoded), 56);
    const SubrecordSlice* inten = FindSubrecord(
        slices, GSF_SWATH_BATHY_SUBRECORD_INTENSITY_SERIES_ARRAY);
    ASSERT_NE(nullptr, inten);
    EXPECT_EQ(21u + ic.expected_imagery_size + 2u * (12u + 4u), inten->size);
  }

  // 4. Default (UNKNOWN sensor_id) imagery omits sensor imagery header.
  {
    std::vector<unsigned int> samples = {5, 6};
    std::array<gsfTimeSeriesIntensity, 2> series{};
    std::memset(series.data(), 0, series.size() * sizeof(series[0]));
    series[0].sample_count = 2;
    series[0].samples = samples.data();
    series[1].sample_count = 2;
    series[1].samples = samples.data();

    gsfBRBIntensity brb{};
    ZeroStruct(brb);
    brb.bits_per_sample = 8;
    brb.time_series = series.data();
    GSF_FILE_TABLE ft{};
    ZeroStruct(ft);
    ft.major_version_number = 3;
    gsfSwathBathyPing ping;
    InitBasePing(ping, 2, false);
    ping.brb_inten = &brb;
    std::fill(buf.begin(), buf.begin() + 512, 0);
    const int encoded = gsfEncodeSwathBathymetryPing(buf.data(), &ping, &ft);
    ASSERT_GT(encoded, 56);
    auto slices = ParsePingSubrecords(
        std::span<const unsigned char>(buf.data(), encoded), 56);
    const SubrecordSlice* inten = FindSubrecord(
        slices, GSF_SWATH_BATHY_SUBRECORD_INTENSITY_SERIES_ARRAY);
    ASSERT_NE(nullptr, inten);
    EXPECT_EQ(21u + 2u * (12u + 2u), inten->size);
  }

  // 5. Unsupported bits_per_sample and oversized record error paths.
  {
    std::vector<unsigned int> samples = {1, 2};
    std::array<gsfTimeSeriesIntensity, 2> series{};
    std::memset(series.data(), 0, series.size() * sizeof(series[0]));
    series[0].sample_count = 2;
    series[0].samples = samples.data();
    series[1].sample_count = 2;
    series[1].samples = samples.data();

    gsfBRBIntensity bad_bits{};
    ZeroStruct(bad_bits);
    bad_bits.bits_per_sample = 24;
    bad_bits.time_series = series.data();
    GSF_FILE_TABLE ft{};
    ZeroStruct(ft);
    ft.major_version_number = 3;
    gsfSwathBathyPing ping;
    InitBasePing(ping, 2, false);
    ping.brb_inten = &bad_bits;
    gsfError = GSF_NORMAL;
    EXPECT_EQ(-1, gsfEncodeSwathBathymetryPing(buf.data(), &ping, &ft));
    EXPECT_EQ(GSF_MB_PING_RECORD_ENCODE_FAILED, gsfIntError());
  }

  {
    std::vector<unsigned int> large_samples(45000, 1u);
    std::array<gsfTimeSeriesIntensity, 3> series{};
    std::memset(series.data(), 0, series.size() * sizeof(series[0]));
    for (auto& item : series) {
      item.sample_count = static_cast<unsigned short>(large_samples.size());
      item.samples = large_samples.data();
    }

    gsfBRBIntensity too_big{};
    ZeroStruct(too_big);
    too_big.bits_per_sample = 32;
    too_big.time_series = series.data();
    GSF_FILE_TABLE ft{};
    ZeroStruct(ft);
    ft.major_version_number = 3;
    gsfSwathBathyPing ping;
    InitBasePing(ping, 3, false);
    ping.brb_inten = &too_big;
    gsfError = GSF_NORMAL;
    EXPECT_EQ(-1, gsfEncodeSwathBathymetryPing(buf.data(), &ping, &ft));
    EXPECT_EQ(GSF_RECORD_SIZE_ERROR, gsfIntError());
  }
}

TEST(GsfEncTest, EncodeAllSensorSpecificSubrecords) {
  auto buf = MakeEncodeBuffer();
  struct SensorTestCase {
    unsigned int sensor_id;
    uint32_t expected_size = 0;
    int major_version = 3;
    int minor_version = 11;
    bool negative_signed = false;
    bool alt_variant = false;
  };
  constexpr auto kSensorCases = std::to_array<SensorTestCase>({
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_UNKNOWN},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC,
       .expected_size = 2},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM12_SPECIFIC,
       .expected_size = 39},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM100_SPECIFIC,
       .expected_size = 11},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM100_SPECIFIC,
       .expected_size = 11,
       .negative_signed = true},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM950_SPECIFIC,
       .expected_size = 10},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM950_SPECIFIC,
       .expected_size = 10,
       .negative_signed = true},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM1000_SPECIFIC,
       .expected_size = 10},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM1000_SPECIFIC,
       .expected_size = 10,
       .negative_signed = true},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM121A_SPECIFIC,
       .expected_size = 11},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM121_SPECIFIC,
       .expected_size = 11},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_SASS_SPECIFIC,
       .expected_size = 12},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_TYPEIII_SEABEAM_SPECIFIC,
       .expected_size = 12},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_CMP_SASS_SPECIFIC,
       .expected_size = 4},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_SEAMAP_SPECIFIC,
       .expected_size = 22},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_SEAMAP_SPECIFIC,
       .expected_size = 20,
       .major_version = 2,
       .minor_version = 6},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_SEABAT_SPECIFIC,
       .expected_size = 8},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_SB_AMP_SPECIFIC,
       .expected_size = 10},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_SB_AMP_SPECIFIC,
       .expected_size = 10,
       .negative_signed = true},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_SEABAT_II_SPECIFIC,
       .expected_size = 18},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_SEABAT_8101_SPECIFIC,
       .expected_size = 31},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_SEABEAM_2112_SPECIFIC,
       .expected_size = 14},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_ELAC_MKII_SPECIFIC,
       .expected_size = 11},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM2000_SPECIFIC,
       .expected_size = 70},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC,
       .expected_size = 70},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM1002_SPECIFIC,
       .expected_size = 70},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM300_SPECIFIC,
       .expected_size = 70},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM120_SPECIFIC,
       .expected_size = 70},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM3002_SPECIFIC,
       .expected_size = 70},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM3000D_SPECIFIC,
       .expected_size = 70},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM3002D_SPECIFIC,
       .expected_size = 70},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM121A_SIS_SPECIFIC,
       .expected_size = 70},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM2000_RAW_SPECIFIC,
       .expected_size = 225},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM3000_RAW_SPECIFIC,
       .expected_size = 225,
       .negative_signed = true},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM1002_RAW_SPECIFIC,
       .expected_size = 225,
       .alt_variant = true},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM300_RAW_SPECIFIC,
       .expected_size = 225,
       .negative_signed = true},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM120_RAW_SPECIFIC,
       .expected_size = 225},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM3002_RAW_SPECIFIC,
       .expected_size = 225},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM3000D_RAW_SPECIFIC,
       .expected_size = 225},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM3002D_RAW_SPECIFIC,
       .expected_size = 225},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM121A_SIS_RAW_SPECIFIC,
       .expected_size = 225},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM710_SPECIFIC,
       .expected_size = 230},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM302_SPECIFIC,
       .expected_size = 230,
       .negative_signed = true},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM122_SPECIFIC,
       .expected_size = 230},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM2040_SPECIFIC,
       .expected_size = 230},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_ME70BO_SPECIFIC,
       .expected_size = 230,
       .negative_signed = true},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC,
       .expected_size = 473,
       .alt_variant = true},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC,
       .expected_size = 473,
       .negative_signed = true},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_GEOSWATH_PLUS_SPECIFIC,
       .expected_size = 82},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_KLEIN_5410_BSS_SPECIFIC,
       .expected_size = 86},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
       .expected_size = 52},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8111_SPECIFIC,
       .expected_size = 52},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8124_SPECIFIC,
       .expected_size = 52},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8125_SPECIFIC,
       .expected_size = 52},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8150_SPECIFIC,
       .expected_size = 52},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8160_SPECIFIC,
       .expected_size = 52},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC,
       .expected_size = 186},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_TSERIES_SPECIFIC,
       .expected_size = 715},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_TSERIES_SPECIFIC,
       .expected_size = 715,
       .negative_signed = true},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_DELTA_T_SPECIFIC,
       .expected_size = 93},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2020_SPECIFIC,
       .expected_size = 204},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2022_SPECIFIC,
       .expected_size = 204},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2024_SPECIFIC,
       .expected_size = 204},
      {.sensor_id = GSF_SWATH_BATHY_SB_SUBRECORD_ECHOTRAC_SPECIFIC,
       .expected_size = 10},
      {.sensor_id = GSF_SWATH_BATHY_SB_SUBRECORD_ECHOTRAC_SPECIFIC,
       .expected_size = 10,
       .negative_signed = true},
      {.sensor_id = GSF_SWATH_BATHY_SB_SUBRECORD_BATHY2000_SPECIFIC,
       .expected_size = 10},
      {.sensor_id = GSF_SWATH_BATHY_SB_SUBRECORD_MGD77_SPECIFIC,
       .expected_size = 18},
      {.sensor_id = GSF_SWATH_BATHY_SB_SUBRECORD_BDB_SPECIFIC,
       .expected_size = 14},
      {.sensor_id = GSF_SWATH_BATHY_SB_SUBRECORD_NOSHDB_SPECIFIC,
       .expected_size = 8},
      {.sensor_id = GSF_SWATH_BATHY_SB_SUBRECORD_PDD_SPECIFIC,
       .expected_size = 10},
      {.sensor_id = GSF_SWATH_BATHY_SB_SUBRECORD_NAVISOUND_SPECIFIC,
       .expected_size = 10},
  });

  for (const auto& tc : kSensorCases) {
    SCOPED_TRACE(::testing::Message()
                 << "sensor_id=" << tc.sensor_id
                 << " negative_signed=" << tc.negative_signed
                 << " alt_variant=" << tc.alt_variant);
    std::fill(buf.begin(), buf.begin() + 2048, 0);
    GSF_FILE_TABLE ft{};
    ZeroStruct(ft);
    ft.major_version_number = tc.major_version;
    ft.minor_version_number = tc.minor_version;

    gsfSwathBathyPing ping;
    InitBasePing(ping, 4, tc.negative_signed);
    ping.sensor_id = static_cast<int>(tc.sensor_id);
    PopulateSensorDataForTest(ping, tc.sensor_id, tc.negative_signed,
                              tc.alt_variant);

    const int encoded = gsfEncodeSwathBathymetryPing(buf.data(), &ping, &ft);
    const size_t header_bytes = tc.major_version > 2 ? 56u : 42u;
    ASSERT_GT(encoded, static_cast<int>(header_bytes));

    auto slices = ParsePingSubrecords(
        std::span<const unsigned char>(buf.data(), encoded), header_bytes);
    ASSERT_EQ(1u, slices.size());
    EXPECT_EQ(static_cast<uint8_t>(tc.sensor_id), slices[0].id);
    EXPECT_EQ(tc.expected_size, slices[0].size);

    if (tc.negative_signed || tc.alt_variant) {
      GSF_FILE_TABLE dec_ft{};
      ZeroStruct(dec_ft);
      dec_ft.major_version_number = tc.major_version;
      dec_ft.minor_version_number = tc.minor_version;
      absl::Cleanup reset_dec([&dec_ft] { ResetDecodeState(dec_ft); });
      gsfSwathBathyPing decoded_ping{};
      ZeroStruct(decoded_ping);
      ASSERT_EQ(encoded,
                gsfDecodeSwathBathymetryPing(&decoded_ping, buf.data(), &dec_ft,
                                             kValidDecodeHandle, encoded));
      switch (tc.sensor_id) {
        case GSF_SWATH_BATHY_SUBRECORD_EM100_SPECIFIC:
          EXPECT_LT(decoded_ping.sensor_data.gsfEM100Specific.ship_pitch, 0.0);
          break;
        case GSF_SWATH_BATHY_SUBRECORD_EM950_SPECIFIC:
          EXPECT_LT(decoded_ping.sensor_data.gsfEM950Specific.ship_pitch, 0.0);
          break;
        case GSF_SWATH_BATHY_SUBRECORD_EM1000_SPECIFIC:
          EXPECT_LT(decoded_ping.sensor_data.gsfEM1000Specific.ship_pitch, 0.0);
          break;
        case GSF_SWATH_BATHY_SUBRECORD_SB_AMP_SPECIFIC:
          EXPECT_EQ(-15,
                    decoded_ping.sensor_data.gsfSBAmpSpecific.avg_gate_depth);
          break;
        case GSF_SWATH_BATHY_SUBRECORD_EM3000_RAW_SPECIFIC:
        case GSF_SWATH_BATHY_SUBRECORD_EM1002_RAW_SPECIFIC:
        case GSF_SWATH_BATHY_SUBRECORD_EM300_RAW_SPECIFIC:
          if (tc.alt_variant) {
            EXPECT_EQ(
                1002, decoded_ping.sensor_data.gsfEM3RawSpecific.model_number);
          } else {
            EXPECT_LT(
                decoded_ping.sensor_data.gsfEM3RawSpecific.sector[0].tilt_angle,
                0.0);
          }
          break;
        case GSF_SWATH_BATHY_SUBRECORD_EM302_SPECIFIC:
        case GSF_SWATH_BATHY_SUBRECORD_ME70BO_SPECIFIC:
          EXPECT_LT(
              decoded_ping.sensor_data.gsfEM4Specific.sector[0].tilt_angle,
              0.0);
          break;
        case GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC:
          if (tc.alt_variant) {
            EXPECT_GT(
                decoded_ping.sensor_data.gsfKMALLSpecific.frequencyMode_Hz,
                40000.0);
          } else {
            EXPECT_LT(
                decoded_ping.sensor_data.gsfKMALLSpecific.portSectorEdge_deg,
                0.0);
          }
          break;
        case GSF_SWATH_BATHY_SUBRECORD_RESON_TSERIES_SPECIFIC:
          EXPECT_LT(
              decoded_ping.sensor_data.gsfResonTSeriesSpecific.depth_filt_min,
              0.0);
          break;
        case GSF_SWATH_BATHY_SB_SUBRECORD_ECHOTRAC_SPECIFIC:
          EXPECT_LT(
              decoded_ping.sensor_data.gsfSBEchotracSpecific.dynamic_draft,
              0.0);
          break;
        default:
          FAIL() << "Unhandled negative/alt assertion for sensor "
                 << tc.sensor_id;
      }
    }
  }
}

// TODO(schwehr): Revisit dead branches in gsf_enc.c (e.g. commented-out
// dual-head EM3 run_time[1] encoding and unused ucptr handling in
// gsfSetDefaultScaleFactor) and negative float-to-unsigned casts that fail
// UBSan float-cast-overflow.

}  // namespace
}  // namespace gsf
