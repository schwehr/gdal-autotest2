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
// Tests GSF in-memory record and subrecord decoding (src/gsf_dec.c).
//
// In google3 Linux builds, _LARGEFILE64_SOURCE is enabled and WIN32/WIN64
// branches are compiled out. Keep 1-byte signed char-decoded fields in 0..127
// so behavior matches across -funsigned-char and --cpu=arm builds.
//
// See also:
//   https://github.com/schwehr/generic-sensor-format-3/blob/main/src/gsf_dec.c
//   https://github.com/schwehr/generic-sensor-format-3/blob/main/src/gsf_dec.h
//   https://github.com/schwehr/generic-sensor-format-3/blob/main/src/gsf.h

#include "third_party/gsf/src/gsf.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gsf/google/util/gsf_internal.h"

namespace gsf {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::NotNull;

constexpr int kHandle = 1;
constexpr std::size_t kEncodeBufSize = 65536;

// Owns a stack GSF_FILE_TABLE for direct gsfDecode* calls and ensures all
// dynamic allocations in ft.rec and static arraySize[handle - 1] state are
// freed and reset on every exit path.
struct ScopedDecodeTable {
  GSF_FILE_TABLE ft;
  int handle = kHandle;

  explicit ScopedDecodeTable(int major = 3, int minor = 11, int h = kHandle)
      : handle(h) {
    std::memset(&ft, 0, sizeof(ft));
    ft.major_version_number = major;
    ft.minor_version_number = minor;
  }

  ~ScopedDecodeTable() {
    // gsfFree loops over ft.rec.mb_ping.number_beams to free
    // brb_inten->time_series[i].samples, while gsfDecodeSwathBathymetryPing
    // writes number_beams only to the caller's ping struct and arraySize.
    if (ft.rec.mb_ping.brb_inten != nullptr) {
      ft.rec.mb_ping.number_beams =
          arraySize[handle - 1]
                   [GSF_SWATH_BATHY_SUBRECORD_INTENSITY_SERIES_ARRAY - 1];
    }
    gsfFree(&ft.rec);
    std::fill(std::begin(arraySize[handle - 1]),
              std::end(arraySize[handle - 1]), short{0});
    gsfError = GSF_NORMAL;
  }

  ScopedDecodeTable(const ScopedDecodeTable&) = delete;
  ScopedDecodeTable& operator=(const ScopedDecodeTable&) = delete;
};

void AppendU8(std::vector<unsigned char>& out, std::uint8_t value) {
  out.push_back(static_cast<unsigned char>(value));
}

void AppendU16Be(std::vector<unsigned char>& out, std::uint16_t value) {
  out.push_back(static_cast<unsigned char>((value >> 8) & 0xFFu));
  out.push_back(static_cast<unsigned char>(value & 0xFFu));
}

void AppendI16Be(std::vector<unsigned char>& out, std::int16_t value) {
  AppendU16Be(out, static_cast<std::uint16_t>(value));
}

void AppendU32Be(std::vector<unsigned char>& out, std::uint32_t value) {
  out.push_back(static_cast<unsigned char>((value >> 24) & 0xFFu));
  out.push_back(static_cast<unsigned char>((value >> 16) & 0xFFu));
  out.push_back(static_cast<unsigned char>((value >> 8) & 0xFFu));
  out.push_back(static_cast<unsigned char>(value & 0xFFu));
}

void AppendI32Be(std::vector<unsigned char>& out, std::int32_t value) {
  AppendU32Be(out, static_cast<std::uint32_t>(value));
}

void AppendSubrecordHeader(std::vector<unsigned char>& out, unsigned int id,
                           unsigned int size) {
  AppendU32Be(out, ((static_cast<std::uint32_t>(id) & 0xFFu) << 24) |
                       (static_cast<std::uint32_t>(size) & 0x00FFFFFFu));
}

void AppendPingHeaderV3(std::vector<unsigned char>& out,
                        std::int16_t num_beams) {
  AppendU32Be(out, 1700000000u);  // ping_time.tv_sec
  AppendU32Be(out, 123456000u);   // ping_time.tv_nsec
  AppendI32Be(out, -1224194000);  // longitude (-122.4194 deg)
  AppendI32Be(out, 377749000);    // latitude (37.7749 deg)
  AppendI16Be(out, num_beams);    // number_beams
  AppendI16Be(out, 1);            // center_beam
  AppendU16Be(out, 0x0001u);      // ping_flags
  AppendI16Be(out, 0);            // reserved
  AppendI16Be(out, -50);          // tide_corrector (-0.50 m)
  AppendI32Be(out, 250);          // depth_corrector (2.50 m)
  AppendU16Be(out, 18000);        // heading (180.00 deg)
  AppendI16Be(out, -125);         // pitch (-1.25 deg)
  AppendI16Be(out, 234);          // roll (2.34 deg)
  AppendI16Be(out, -35);          // heave (-0.35 m)
  AppendU16Be(out, 18150);        // course (181.50 deg)
  AppendU16Be(out, 850);          // speed (8.50 knots)
  AppendI32Be(out, 12345);        // height (12.345 m)
  AppendI32Be(out, -2345);        // sep (-2.345 m)
  AppendI32Be(out, 456);          // gps_tide_corrector (0.456 m)
  AppendU16Be(out, 0);            // spare
}

gsfSwathBathyPing MakeBasePing(short num_beams, int sensor_id) {
  gsfSwathBathyPing ping;
  std::memset(&ping, 0, sizeof(ping));
  ping.ping_time.tv_sec = 1700000100;
  ping.ping_time.tv_nsec = 250000000;
  ping.latitude = 36.8012345;
  ping.longitude = -121.9012345;
  ping.height = 14.321;
  ping.sep = -1.234;
  ping.number_beams = num_beams;
  ping.center_beam = num_beams > 0 ? num_beams / 2 : 0;
  ping.ping_flags = GSF_PING_USER_FLAG_01;
  ping.reserved = 7;
  ping.tide_corrector = -1.25;
  ping.gps_tide_corrector = 0.678;
  ping.depth_corrector = 3.45;
  ping.heading = 275.25;
  ping.pitch = -2.15;
  ping.roll = 3.25;
  ping.heave = -0.45;
  ping.course = 276.50;
  ping.speed = 9.75;
  ping.sensor_id = sensor_id;
  return ping;
}

std::vector<unsigned char> EncodePing(gsfSwathBathyPing& ping, int major = 3,
                                      int minor = 11) {
  std::vector<unsigned char> buf(kEncodeBufSize, 0);
  GSF_FILE_TABLE enc_ft;
  std::memset(&enc_ft, 0, sizeof(enc_ft));
  enc_ft.major_version_number = major;
  enc_ft.minor_version_number = minor;
  const int encoded = gsfEncodeSwathBathymetryPing(buf.data(), &ping, &enc_ft);
  EXPECT_GT(encoded, 0);
  if (encoded <= 0) {
    return {};
  }
  buf.resize(static_cast<std::size_t>(encoded));
  return buf;
}

// Copies `sptr[0..record_size)` into a buffer with 16 zero-initialized guard
// bytes (matching gsf.c's 512 KiB static streamBuff) because gsf_dec.c:861-867's
// lookahead (`record_size - bytes - sr_size > 4` with `bytes == p - sptr - 4`)
// reads up to 3 bytes past `sptr + record_size`.
int DecodeSwathPingPadded(gsfSwathBathyPing* ping, const unsigned char* sptr,
                          GSF_FILE_TABLE* ft, int handle, int record_size) {
  if (record_size <= 0 || sptr == nullptr) {
    return -1;
  }
  std::vector<unsigned char> padded(static_cast<std::size_t>(record_size) + 16u,
                                    0);
  std::memcpy(padded.data(), sptr, static_cast<std::size_t>(record_size));
  return gsfDecodeSwathBathymetryPing(ping, padded.data(), ft, handle,
                                      record_size);
}

void LoadScaledFactor(gsfSwathBathyPing& ping, unsigned int subrecord_id,
                      char compression_flag, double precision, int offset = 0) {
  ASSERT_EQ(GSF_NORMAL,
            gsfLoadScaleFactor(&ping.scaleFactors, subrecord_id,
                               compression_flag, precision, offset));
}

void PopulateEmRunTime(t_gsfEMRunTime& rt, int model_number) {
  rt.model_number = model_number;
  rt.dg_time.tv_sec = 1700000200;
  rt.dg_time.tv_nsec = 500000000;
  rt.ping_counter = 4321;
  rt.serial_number = 987;
  rt.operator_station_status = 1;
  rt.processing_unit_status = 2;
  rt.bsp_status = 3;
  rt.head_transceiver_status = 4;
  rt.mode = GSF_EM_MODE_SHALLOW;
  rt.filter_id = 5;
  rt.min_depth = 10.0;
  rt.max_depth = 500.0;
  rt.absorption = 22.5;
  rt.tx_pulse_length = 150.0;
  rt.tx_beam_width = 1.2;
  rt.tx_power_re_max = 10.0;
  rt.rx_beam_width = 1.5;
  rt.rx_bandwidth = 2500.0;
  rt.rx_fixed_gain = 12.0;
  rt.tvg_cross_over_angle = 45.0;
  rt.ssv_source = 1;
  rt.max_port_swath_width = 300;
  rt.beam_spacing = 2;
  rt.max_port_coverage = 60;
  rt.stabilization = 3;
  rt.max_stbd_coverage = 65;
  rt.max_stbd_swath_width = 320;
  rt.durotong_speed = 1495.5;
  rt.hi_low_absorption_ratio = 18.0;
  rt.tx_along_tilt = -2.25;
  rt.filter_id_2 = 2;
}

void PopulateEmPuStatus(t_gsfEMPUStatus& pu) {
  pu.pu_cpu_load = 42.0;
  pu.sensor_status = GSF_EM_VALID_1_PPS | GSF_EM_VALID_POSITION;
  pu.achieved_port_coverage = 58;
  pu.achieved_stbd_coverage = 62;
  pu.yaw_stabilization = -1.75;
}

TEST(GsfDecTest, DecodeHeaderRoundTripAndTruncation) {
  // 1. Round-trip standard version string.
  gsfHeader encoded_header{};
  std::vector<unsigned char> stream(GSF_VERSION_SIZE, 0);
  ASSERT_EQ(GSF_VERSION_SIZE, gsfEncodeHeader(stream.data(), &encoded_header));

  gsfHeader decoded_header{};
  const int decoded_len = gsfDecodeHeader(&decoded_header, stream.data());
  EXPECT_EQ(static_cast<int>(std::string_view(GSF_VERSION).size()),
            decoded_len);
  EXPECT_STREQ(GSF_VERSION, decoded_header.version);

  // 2. Non-NUL-terminated 12-byte input must be NUL-terminated at index 11.
  const std::array<unsigned char, GSF_VERSION_SIZE> unterminated = {
      'G', 'S', 'F', '-', 'v', '0', '3', '.', '1', '1', 'X', 'Y'};
  gsfHeader truncated{};
  EXPECT_EQ(GSF_VERSION_SIZE - 1,
            gsfDecodeHeader(&truncated, unterminated.data()));
  EXPECT_STREQ("GSF-v03.11X", truncated.version);
}

TEST(GsfDecTest, DecodeSwathBathySummaryRoundTrip) {
  gsfSwathBathySummary in{};
  in.start_time.tv_sec = 1700000000;
  in.start_time.tv_nsec = 111222333;
  in.end_time.tv_sec = 1700003600;
  in.end_time.tv_nsec = 444555666;
  in.min_latitude = -45.1234567;
  in.min_longitude = -170.7654321;
  in.max_latitude = 12.3456789;
  in.max_longitude = 172.9876543;
  in.min_depth = -1.25;
  in.max_depth = 4500.75;

  std::vector<unsigned char> stream(kEncodeBufSize, 0);
  const int encoded = gsfEncodeSwathBathySummary(stream.data(), &in);
  ASSERT_EQ(40, encoded);

  gsfSwathBathySummary out{};
  EXPECT_EQ(40, gsfDecodeSwathBathySummary(&out, stream.data()));
  EXPECT_EQ(in.start_time.tv_sec, out.start_time.tv_sec);
  EXPECT_EQ(in.start_time.tv_nsec, out.start_time.tv_nsec);
  EXPECT_EQ(in.end_time.tv_sec, out.end_time.tv_sec);
  EXPECT_EQ(in.end_time.tv_nsec, out.end_time.tv_nsec);
  EXPECT_NEAR(in.min_latitude, out.min_latitude, 1e-7);
  EXPECT_NEAR(in.min_longitude, out.min_longitude, 1e-7);
  EXPECT_NEAR(in.max_latitude, out.max_latitude, 1e-7);
  EXPECT_NEAR(in.max_longitude, out.max_longitude, 1e-7);
  EXPECT_NEAR(in.min_depth, out.min_depth, 1e-2);
  EXPECT_NEAR(in.max_depth, out.max_depth, 1e-2);
}

TEST(GsfDecTest, DecodeSinglebeamAllSensorsAndUnknown) {
  auto make_sb_ping = [](int sensor_id) {
    gsfSingleBeamPing ping;
    std::memset(&ping, 0, sizeof(ping));
    ping.ping_time.tv_sec = 1700001000;
    ping.ping_time.tv_nsec = 333000000;
    ping.longitude = -75.1234567;
    ping.latitude = 38.7654321;
    ping.tide_corrector = -0.45;
    ping.depth_corrector = 1.75;
    ping.heading = 92.35;
    ping.pitch = -1.50;
    ping.roll = 2.25;
    ping.heave = -0.15;
    ping.depth = 34.56;
    ping.sound_speed_correction = 0.85;
    ping.positioning_system_type = 4;
    ping.sensor_id = sensor_id;
    return ping;
  };

  auto check_common = [](const gsfSingleBeamPing& expected,
                         const gsfSingleBeamPing& actual) {
    EXPECT_EQ(expected.ping_time.tv_sec, actual.ping_time.tv_sec);
    EXPECT_EQ(expected.ping_time.tv_nsec, actual.ping_time.tv_nsec);
    EXPECT_NEAR(expected.longitude, actual.longitude, 1e-7);
    EXPECT_NEAR(expected.latitude, actual.latitude, 1e-7);
    EXPECT_NEAR(expected.tide_corrector, actual.tide_corrector, 1e-2);
    EXPECT_NEAR(expected.depth_corrector, actual.depth_corrector, 1e-2);
    EXPECT_NEAR(expected.heading, actual.heading, 1e-2);
    EXPECT_NEAR(expected.pitch, actual.pitch, 1e-2);
    EXPECT_NEAR(expected.roll, actual.roll, 1e-2);
    EXPECT_NEAR(expected.heave, actual.heave, 1e-2);
    EXPECT_NEAR(expected.depth, actual.depth, 1e-2);
    EXPECT_NEAR(expected.sound_speed_correction, actual.sound_speed_correction,
                1e-2);
    EXPECT_EQ(expected.positioning_system_type, actual.positioning_system_type);
    EXPECT_EQ(expected.sensor_id, actual.sensor_id);
  };

  // 1. Echotrac and Bathy2000 share DecodeEchotracSpecific.
  constexpr int kEchotracIds[] = {
      GSF_SINGLE_BEAM_SUBRECORD_ECHOTRAC_SPECIFIC,
      GSF_SINGLE_BEAM_SUBRECORD_BATHY2000_SPECIFIC,
  };
  for (int sensor_id : kEchotracIds) {
    gsfSingleBeamPing in = make_sb_ping(sensor_id);
    in.sensor_data.gsfEchotracSpecific.navigation_error = 12;
    in.sensor_data.gsfEchotracSpecific.mpp_source =
        GSF_SB_MPP_SOURCE_DGPS_TRIMBLE;
    in.sensor_data.gsfEchotracSpecific.tide_source = 2;

    std::vector<unsigned char> buf(256, 0);
    const int enc = gsfEncodeSinglebeam(buf.data(), &in);
    ASSERT_GT(enc, 0);

    gsfSingleBeamPing out;
    std::memset(&out, 0, sizeof(out));
    EXPECT_EQ(enc, gsfDecodeSinglebeam(&out, buf.data(), enc));
    check_common(in, out);
    EXPECT_EQ(12, out.sensor_data.gsfEchotracSpecific.navigation_error);
    EXPECT_EQ(GSF_SB_MPP_SOURCE_DGPS_TRIMBLE,
              out.sensor_data.gsfEchotracSpecific.mpp_source);
    EXPECT_EQ(2, out.sensor_data.gsfEchotracSpecific.tide_source);
  }

  // 2. MGD77.
  {
    gsfSingleBeamPing in =
        make_sb_ping(GSF_SINGLE_BEAM_SUBRECORD_MGD77_SPECIFIC);
    in.sensor_data.gsfMGD77Specific.time_zone_corr = 5;
    in.sensor_data.gsfMGD77Specific.position_type_code = 3;
    in.sensor_data.gsfMGD77Specific.correction_code = 7;
    in.sensor_data.gsfMGD77Specific.bathy_type_code = 2;
    in.sensor_data.gsfMGD77Specific.quality_code = 1;
    in.sensor_data.gsfMGD77Specific.travel_time = 0.0461;

    std::vector<unsigned char> buf(256, 0);
    const int enc = gsfEncodeSinglebeam(buf.data(), &in);
    ASSERT_GT(enc, 0);

    gsfSingleBeamPing out;
    std::memset(&out, 0, sizeof(out));
    EXPECT_EQ(enc, gsfDecodeSinglebeam(&out, buf.data(), enc));
    check_common(in, out);
    EXPECT_EQ(5, out.sensor_data.gsfMGD77Specific.time_zone_corr);
    EXPECT_EQ(3, out.sensor_data.gsfMGD77Specific.position_type_code);
    EXPECT_EQ(7, out.sensor_data.gsfMGD77Specific.correction_code);
    EXPECT_EQ(2, out.sensor_data.gsfMGD77Specific.bathy_type_code);
    EXPECT_EQ(1, out.sensor_data.gsfMGD77Specific.quality_code);
    EXPECT_NEAR(0.0461, out.sensor_data.gsfMGD77Specific.travel_time, 1e-4);
  }

  // 3. BDB.
  {
    gsfSingleBeamPing in = make_sb_ping(GSF_SINGLE_BEAM_SUBRECORD_BDB_SPECIFIC);
    in.sensor_data.gsfBDBSpecific.doc_no = 54321;
    in.sensor_data.gsfBDBSpecific.eval = '1';
    in.sensor_data.gsfBDBSpecific.classification = 'U';
    in.sensor_data.gsfBDBSpecific.track_adj_flag = 'Y';
    in.sensor_data.gsfBDBSpecific.source_flag = 'S';
    in.sensor_data.gsfBDBSpecific.pt_or_track_ln = 'T';
    in.sensor_data.gsfBDBSpecific.datum_flag = 'W';

    std::vector<unsigned char> buf(256, 0);
    const int enc = gsfEncodeSinglebeam(buf.data(), &in);
    ASSERT_GT(enc, 0);

    gsfSingleBeamPing out;
    std::memset(&out, 0, sizeof(out));
    EXPECT_EQ(enc, gsfDecodeSinglebeam(&out, buf.data(), enc));
    check_common(in, out);
    EXPECT_EQ(54321, out.sensor_data.gsfBDBSpecific.doc_no);
    EXPECT_EQ('1', out.sensor_data.gsfBDBSpecific.eval);
    EXPECT_EQ('U', out.sensor_data.gsfBDBSpecific.classification);
    EXPECT_EQ('Y', out.sensor_data.gsfBDBSpecific.track_adj_flag);
    EXPECT_EQ('S', out.sensor_data.gsfBDBSpecific.source_flag);
    EXPECT_EQ('T', out.sensor_data.gsfBDBSpecific.pt_or_track_ln);
    EXPECT_EQ('W', out.sensor_data.gsfBDBSpecific.datum_flag);
  }

  // 4. NOSHDB.
  {
    gsfSingleBeamPing in =
        make_sb_ping(GSF_SINGLE_BEAM_SUBRECORD_NOSHDB_SPECIFIC);
    in.sensor_data.gsfNOSHDBSpecific.type_code = 11;
    in.sensor_data.gsfNOSHDBSpecific.carto_code = 22;

    std::vector<unsigned char> buf(256, 0);
    const int enc = gsfEncodeSinglebeam(buf.data(), &in);
    ASSERT_GT(enc, 0);

    gsfSingleBeamPing out;
    std::memset(&out, 0, sizeof(out));
    EXPECT_EQ(enc, gsfDecodeSinglebeam(&out, buf.data(), enc));
    check_common(in, out);
    EXPECT_EQ(11, out.sensor_data.gsfNOSHDBSpecific.type_code);
    EXPECT_EQ(22, out.sensor_data.gsfNOSHDBSpecific.carto_code);
  }

  // 5. UNKNOWN sensor: both exact size (trailing 4-byte extraction) and
  //    4-byte padded size (while-loop switch case).
  {
    gsfSingleBeamPing in = make_sb_ping(GSF_SWATH_BATHY_SUBRECORD_UNKNOWN);
    std::vector<unsigned char> buf(256, 0);
    const int enc = gsfEncodeSinglebeam(buf.data(), &in);
    ASSERT_GT(enc, 0);

    gsfSingleBeamPing out_exact;
    std::memset(&out_exact, 0, sizeof(out_exact));
    out_exact.sensor_id = 99;
    EXPECT_EQ(enc, gsfDecodeSinglebeam(&out_exact, buf.data(), enc));
    EXPECT_EQ(GSF_SWATH_BATHY_SUBRECORD_UNKNOWN, out_exact.sensor_id);

    gsfSingleBeamPing out_padded;
    std::memset(&out_padded, 0, sizeof(out_padded));
    out_padded.sensor_id = 99;
    EXPECT_EQ(enc, gsfDecodeSinglebeam(&out_padded, buf.data(), enc + 4));
    EXPECT_EQ(GSF_SWATH_BATHY_SUBRECORD_UNKNOWN, out_padded.sensor_id);
  }
}

TEST(GsfDecTest, DecodeSinglebeamUnknownSubrecordSkipAndTruncated) {
  gsfError = GSF_NORMAL;

  // 1. Build a singlebeam record with an unknown subrecord (id=154, size=4)
  //    followed by a valid NOSHDB subrecord (id=205, size=4).
  gsfSingleBeamPing base;
  std::memset(&base, 0, sizeof(base));
  base.sensor_id = GSF_SINGLE_BEAM_SUBRECORD_NOSHDB_SPECIFIC;
  base.sensor_data.gsfNOSHDBSpecific.type_code = 9;
  base.sensor_data.gsfNOSHDBSpecific.carto_code = 10;

  std::vector<unsigned char> encoded(256, 0);
  const int enc = gsfEncodeSinglebeam(encoded.data(), &base);
  ASSERT_EQ(46, enc);  // 38-byte header + 4-byte subrec header + 4-byte payload

  std::vector<unsigned char> with_unknown(encoded.begin(),
                                          encoded.begin() + 38);
  AppendSubrecordHeader(with_unknown, 154, 4);
  AppendU32Be(with_unknown, 0x11223344u);
  with_unknown.insert(with_unknown.end(), encoded.begin() + 38,
                      encoded.begin() + enc);

  gsfSingleBeamPing out;
  std::memset(&out, 0, sizeof(out));
  EXPECT_EQ(static_cast<int>(with_unknown.size()),
            gsfDecodeSinglebeam(&out, with_unknown.data(),
                                static_cast<int>(with_unknown.size())));
  EXPECT_EQ(GSF_UNRECOGNIZED_SUBRECORD_ID, gsfIntError());
  EXPECT_EQ(GSF_SINGLE_BEAM_SUBRECORD_NOSHDB_SPECIFIC, out.sensor_id);
  EXPECT_EQ(9, out.sensor_data.gsfNOSHDBSpecific.type_code);
  EXPECT_EQ(10, out.sensor_data.gsfNOSHDBSpecific.carto_code);

  // 2. Truncated unknown subrecord whose declared size exceeds record_size.
  gsfError = GSF_NORMAL;
  std::vector<unsigned char> truncated(encoded.begin(), encoded.begin() + 38);
  AppendSubrecordHeader(truncated, 154, 64);
  AppendU32Be(truncated, 0xAABBCCDDu);

  gsfSingleBeamPing fail_ping;
  std::memset(&fail_ping, 0, sizeof(fail_ping));
  EXPECT_EQ(-1, gsfDecodeSinglebeam(&fail_ping, truncated.data(),
                                    static_cast<int>(truncated.size())));
  EXPECT_EQ(GSF_UNRECOGNIZED_SUBRECORD_ID, gsfIntError());
  gsfError = GSF_NORMAL;
}

TEST(GsfDecTest, DecodeSoundVelocityProfileRoundTripAndRealloc) {
  ScopedDecodeTable dec;

  auto run_svp = [&](int n_pts, double base_depth, double base_speed) {
    std::vector<double> depths(n_pts);
    std::vector<double> speeds(n_pts);
    for (int i = 0; i < n_pts; ++i) {
      depths[i] = base_depth + i * 10.25;
      speeds[i] = base_speed + i * 0.55;
    }
    gsfSVP in{};
    in.observation_time.tv_sec = 1700001000;
    in.observation_time.tv_nsec = 123000000;
    in.application_time.tv_sec = 1700001100;
    in.application_time.tv_nsec = 456000000;
    in.latitude = -33.8567891;
    in.longitude = 151.2152967;
    in.number_points = n_pts;
    in.depth = depths.data();
    in.sound_speed = speeds.data();

    std::vector<unsigned char> buf(1024, 0);
    const int enc = gsfEncodeSoundVelocityProfile(buf.data(), &in);
    ASSERT_GT(enc, 0);

    gsfSVP out{};
    EXPECT_EQ(enc, gsfDecodeSoundVelocityProfile(&out, &dec.ft, buf.data()));
    EXPECT_EQ(in.observation_time.tv_sec, out.observation_time.tv_sec);
    EXPECT_EQ(in.observation_time.tv_nsec, out.observation_time.tv_nsec);
    EXPECT_EQ(in.application_time.tv_sec, out.application_time.tv_sec);
    EXPECT_EQ(in.application_time.tv_nsec, out.application_time.tv_nsec);
    EXPECT_NEAR(in.latitude, out.latitude, 1e-7);
    EXPECT_NEAR(in.longitude, out.longitude, 1e-7);
    ASSERT_EQ(n_pts, out.number_points);
    for (int i = 0; i < n_pts; ++i) {
      EXPECT_NEAR(depths[i], out.depth[i], 1e-2);
      EXPECT_NEAR(speeds[i], out.sound_speed[i], 1e-2);
    }
  };

  // 1. Initial allocation, growth reallocation, then reuse of larger buffer.
  run_svp(2, 0.0, 1480.0);
  run_svp(5, 5.5, 1500.25);
  run_svp(3, 1.0, 1510.10);
}

TEST(GsfDecTest, DecodeProcessingParametersRoundTripAndRealloc) {
  ScopedDecodeTable dec;

  auto run_params = [&](std::vector<std::string> values) {
    gsfProcessingParameters in{};
    in.param_time.tv_sec = 1700002000;
    in.param_time.tv_nsec = 789000000;
    in.number_parameters = static_cast<int>(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
      in.param[i] = values[i].data();
    }

    std::vector<unsigned char> buf(2048, 0);
    const int enc = gsfEncodeProcessingParameters(buf.data(), &in);
    ASSERT_GT(enc, 0);

    gsfProcessingParameters out{};
    EXPECT_EQ(enc, gsfDecodeProcessingParameters(&out, &dec.ft, buf.data()));
    EXPECT_EQ(in.param_time.tv_sec, out.param_time.tv_sec);
    EXPECT_EQ(in.param_time.tv_nsec, out.param_time.tv_nsec);
    ASSERT_EQ(static_cast<int>(values.size()), out.number_parameters);
    for (std::size_t i = 0; i < values.size(); ++i) {
      ASSERT_THAT(out.param[i], NotNull());
      EXPECT_STREQ(values[i].c_str(), out.param[i]);
    }
  };

  // 1. Initial short param -> grow count and param[0] length -> shrink.
  run_params({"A=1"});
  run_params({"ROLL_COMPENSATED=TRUE_AND_LONGER_VALUE", "TIDE=MLLW", "C=3"});
  run_params({"X=9"});
}

TEST(GsfDecTest, DecodeSensorParametersRoundTripAndRealloc) {
  ScopedDecodeTable dec;

  auto run_params = [&](std::vector<std::string> values) {
    gsfSensorParameters in{};
    in.param_time.tv_sec = 1700003000;
    in.param_time.tv_nsec = 321000000;
    in.number_parameters = static_cast<int>(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
      in.param[i] = values[i].data();
    }

    std::vector<unsigned char> buf(2048, 0);
    const int enc = gsfEncodeSensorParameters(buf.data(), &in);
    ASSERT_GT(enc, 0);

    gsfSensorParameters out{};
    EXPECT_EQ(enc, gsfDecodeSensorParameters(&out, &dec.ft, buf.data()));
    EXPECT_EQ(in.param_time.tv_sec, out.param_time.tv_sec);
    EXPECT_EQ(in.param_time.tv_nsec, out.param_time.tv_nsec);
    ASSERT_EQ(static_cast<int>(values.size()), out.number_parameters);
    for (std::size_t i = 0; i < values.size(); ++i) {
      ASSERT_THAT(out.param[i], NotNull());
      EXPECT_STREQ(values[i].c_str(), out.param[i]);
    }
  };

  // 1. Initial short param -> grow count and param[0] length -> shrink.
  run_params({"S=1"});
  run_params({"SONAR_SERIAL_NUMBER=EM2040_DUAL_HEAD_00123", "MODE=AUTO"});
  run_params({"K=2"});
}

TEST(GsfDecTest, DecodeCommentRoundTripEmptyAndRealloc) {
  ScopedDecodeTable dec;

  auto run_comment = [&](std::string text) {
    gsfComment in{};
    in.comment_time.tv_sec = 1700004000;
    in.comment_time.tv_nsec = 654000000;
    in.comment_length = static_cast<int>(text.size());
    in.comment = text.data();

    std::vector<unsigned char> buf(512, 0);
    const int enc = gsfEncodeComment(buf.data(), &in);
    ASSERT_GT(enc, 0);

    gsfComment out{};
    EXPECT_EQ(enc, gsfDecodeComment(&out, &dec.ft, buf.data()));
    EXPECT_EQ(in.comment_time.tv_sec, out.comment_time.tv_sec);
    EXPECT_EQ(in.comment_time.tv_nsec, out.comment_time.tv_nsec);
    EXPECT_EQ(in.comment_length, out.comment_length);
    ASSERT_THAT(out.comment, NotNull());
    EXPECT_STREQ(text.c_str(), out.comment);
  };

  // 1. Empty comment -> short -> longer (realloc) -> shorter (reuse).
  run_comment("");
  run_comment("hello");
  run_comment("extended survey calibration comment exceeding initial capacity");
  run_comment("ok");
}

TEST(GsfDecTest, DecodeHistoryRoundTripAndRealloc) {
  ScopedDecodeTable dec;

  auto run_history = [&](std::string host, std::string op, std::string cmd,
                         std::string comment) {
    gsfHistory in{};
    in.history_time.tv_sec = 1700005000;
    in.history_time.tv_nsec = 987000000;
    std::copy_n(host.c_str(), std::min(host.size() + 1, sizeof(in.host_name)),
                in.host_name);
    std::copy_n(op.c_str(), std::min(op.size() + 1, sizeof(in.operator_name)),
                in.operator_name);
    in.command_line = cmd.data();
    in.comment = comment.data();

    std::vector<unsigned char> buf(1024, 0);
    const int enc = gsfEncodeHistory(buf.data(), &in);
    ASSERT_GT(enc, 0);

    gsfHistory out{};
    EXPECT_EQ(enc, gsfDecodeHistory(&out, &dec.ft, buf.data()));
    EXPECT_EQ(in.history_time.tv_sec, out.history_time.tv_sec);
    EXPECT_EQ(in.history_time.tv_nsec, out.history_time.tv_nsec);
    EXPECT_STREQ(host.c_str(), out.host_name);
    EXPECT_STREQ(op.c_str(), out.operator_name);
    ASSERT_THAT(out.command_line, NotNull());
    ASSERT_THAT(out.comment, NotNull());
    EXPECT_STREQ(cmd.c_str(), out.command_line);
    EXPECT_STREQ(comment.c_str(), out.comment);
  };

  // 1. Initial decode -> second decode frees and reallocates command/comment.
  run_history("survey-vessel-1", "watch1", "gsf_proc --sv", "initial run");
  run_history("survey-vessel-2", "watch2", "gsf_proc --tide --filter",
              "reprocessed");
}

TEST(GsfDecTest, DecodeHistoryInvalidFieldLengthsFail) {
  struct HistoryErrorCase {
    std::uint16_t host_len;
    std::uint16_t op_len;
  };
  constexpr HistoryErrorCase kCases[] = {
      {.host_len = 0, .op_len = 4},
      {.host_len = GSF_HOST_NAME_LENGTH, .op_len = 4},
      {.host_len = 4, .op_len = 0},
      {.host_len = 4, .op_len = GSF_OPERATOR_LENGTH},
  };

  for (const auto& tc : kCases) {
    ScopedDecodeTable dec;
    std::vector<unsigned char> raw;
    AppendU32Be(raw, 1700005100u);
    AppendU32Be(raw, 0u);
    AppendU16Be(raw, tc.host_len);
    for (std::uint16_t i = 0; i < tc.host_len; ++i) {
      AppendU8(raw, 'h');
    }
    AppendU16Be(raw, tc.op_len);
    for (std::uint16_t i = 0; i < tc.op_len; ++i) {
      AppendU8(raw, 'o');
    }

    gsfHistory out{};
    EXPECT_EQ(-1, gsfDecodeHistory(&out, &dec.ft, raw.data()));
    EXPECT_EQ(GSF_HISTORY_RECORD_DECODE_FAILED, gsfIntError());
  }
}

TEST(GsfDecTest, DecodeNavigationErrorRoundTripAndNegativeRaw) {
  // 1. Round-trip positive values.
  gsfNavigationError in{};
  in.nav_error_time.tv_sec = 1700006000;
  in.nav_error_time.tv_nsec = 111000000;
  in.record_id = GSF_RECORD_SWATH_BATHYMETRY_PING;
  in.longitude_error = 1.5;
  in.latitude_error = 2.3;

  std::vector<unsigned char> buf(64, 0);
  const int enc = gsfEncodeNavigationError(buf.data(), &in);
  ASSERT_EQ(20, enc);

  gsfNavigationError out{};
  EXPECT_EQ(20, gsfDecodeNavigationError(&out, buf.data()));
  EXPECT_EQ(in.nav_error_time.tv_sec, out.nav_error_time.tv_sec);
  EXPECT_EQ(in.nav_error_time.tv_nsec, out.nav_error_time.tv_nsec);
  EXPECT_EQ(in.record_id, out.record_id);
  EXPECT_NEAR(1.5, out.longitude_error, 1e-6);
  EXPECT_NEAR(2.3, out.latitude_error, 1e-6);

  // 2. Signed negative errors decoded from raw bytes.
  std::vector<unsigned char> raw;
  AppendU32Be(raw, 1700006001u);
  AppendU32Be(raw, 222000000u);
  AppendU32Be(raw, 7u);
  AppendI32Be(raw, -12);
  AppendI32Be(raw, -8);

  gsfNavigationError neg_out{};
  EXPECT_EQ(20, gsfDecodeNavigationError(&neg_out, raw.data()));
  EXPECT_NEAR(-1.2, neg_out.longitude_error, 1e-6);
  EXPECT_NEAR(-0.8, neg_out.latitude_error, 1e-6);
}

TEST(GsfDecTest, DecodeHVNavigationErrorRoundTripAndRealloc) {
  ScopedDecodeTable dec;

  auto run_hv = [&](std::string pos_type, double horz, double vert, double sep,
                    char s0, char s1) {
    gsfHVNavigationError in{};
    in.nav_error_time.tv_sec = 1700007000;
    in.nav_error_time.tv_nsec = 444000000;
    in.record_id = GSF_RECORD_SWATH_BATHYMETRY_PING;
    in.horizontal_error = horz;
    in.vertical_error = vert;
    in.SEP_uncertainty = sep;
    in.spare[0] = s0;
    in.spare[1] = s1;
    in.position_type = pos_type.data();

    std::vector<unsigned char> buf(128, 0);
    const int enc = gsfEncodeHVNavigationError(buf.data(), &in);
    ASSERT_GT(enc, 0);

    gsfHVNavigationError out{};
    EXPECT_EQ(enc, gsfDecodeHVNavigationError(&out, &dec.ft, buf.data()));
    EXPECT_EQ(in.nav_error_time.tv_sec, out.nav_error_time.tv_sec);
    EXPECT_EQ(in.nav_error_time.tv_nsec, out.nav_error_time.tv_nsec);
    EXPECT_EQ(in.record_id, out.record_id);
    EXPECT_NEAR(horz, out.horizontal_error, 1e-3);
    EXPECT_NEAR(vert, out.vertical_error, 1e-3);
    EXPECT_NEAR(sep, out.SEP_uncertainty, 1e-2);
    EXPECT_EQ(s0, out.spare[0]);
    EXPECT_EQ(s1, out.spare[1]);
    ASSERT_THAT(out.position_type, NotNull());
    EXPECT_STREQ(pos_type.c_str(), out.position_type);
  };

  // 1. Initial decode -> second decode frees and reallocates position_type.
  run_hv(GSF_POS_TYPE_GPSU, 1.234, 0.567, 0.89, 'A', 'B');
  run_hv(GSF_POS_TYPE_PIWA, -0.250, -0.125, 0.12, 'C', 'D');
}

TEST(GsfDecTest, DecodeAttitudeRoundTripRolloverAndRealloc) {
  ScopedDecodeTable dec;

  auto run_att = [&](struct timespec base, const std::vector<double>& offsets,
                     std::vector<double> pitch, std::vector<double> roll,
                     std::vector<double> heave, std::vector<double> heading) {
    const int n = static_cast<int>(offsets.size());
    std::vector<struct timespec> times(n);
    for (int i = 0; i < n; ++i) {
      const long total_ns =
          base.tv_nsec + static_cast<long>(offsets[i] * 1e9 + 0.5);
      times[i].tv_sec = base.tv_sec + total_ns / 1000000000L;
      times[i].tv_nsec = total_ns % 1000000000L;
    }

    gsfAttitude in{};
    in.num_measurements = static_cast<short>(n);
    in.attitude_time = times.data();
    in.pitch = pitch.data();
    in.roll = roll.data();
    in.heave = heave.data();
    in.heading = heading.data();

    std::vector<unsigned char> buf(512, 0);
    const int enc = gsfEncodeAttitude(buf.data(), &in);
    ASSERT_GT(enc, 0);

    gsfAttitude out{};
    EXPECT_EQ(enc, gsfDecodeAttitude(&out, &dec.ft, buf.data()));
    ASSERT_EQ(n, out.num_measurements);
    for (int i = 0; i < n; ++i) {
      EXPECT_EQ(times[i].tv_sec, out.attitude_time[i].tv_sec);
      EXPECT_NEAR(static_cast<double>(times[i].tv_nsec),
                  static_cast<double>(out.attitude_time[i].tv_nsec), 1e6);
      EXPECT_NEAR(pitch[i], out.pitch[i], 1e-2);
      EXPECT_NEAR(roll[i], out.roll[i], 1e-2);
      EXPECT_NEAR(heave[i], out.heave[i], 1e-2);
      EXPECT_NEAR(heading[i], out.heading[i], 1e-2);
    }
  };

  // 1. Trigger LocalAddTimes tmp >= 1.0 nanosecond rollover (0.800s + 0.500s).
  run_att({1700008000, 800000000}, {0.0, 0.500}, {-1.25, 0.75}, {2.50, -3.10},
          {-0.20, 0.45}, {90.10, 91.25});

  // 2. Grow num_measurements (2 -> 4) to exercise realloc on all 5 arrays.
  run_att({1700008010, 100000000}, {0.0, 0.1, 0.2, 1.3}, {0.1, 0.2, -0.3, -0.4},
          {1.0, -1.0, 2.0, -2.0}, {0.05, -0.05, 0.15, -0.15},
          {10.0, 10.5, 11.0, 11.5});

  // 3. Reuse previously grown buffers with fewer measurements (4 -> 2).
  run_att({1700008020, 200000000}, {0.0, 0.25}, {0.5, -0.5}, {1.2, -1.2},
          {0.01, -0.01}, {45.0, 45.5});
}

TEST(GsfDecTest, DecodeAttitudeInvalidMeasurementsFail) {
  constexpr std::uint16_t kInvalidCounts[] = {0u, 0xFFFFu};
  for (std::uint16_t raw_count : kInvalidCounts) {
    ScopedDecodeTable dec;
    std::vector<unsigned char> raw;
    AppendU32Be(raw, 1700008100u);
    AppendU32Be(raw, 0u);
    AppendU16Be(raw, raw_count);

    gsfAttitude out{};
    EXPECT_EQ(-1, gsfDecodeAttitude(&out, &dec.ft, raw.data()));
    EXPECT_EQ(GSF_ATTITUDE_RECORD_DECODE_FAILED, gsfIntError());
  }
}

TEST(GsfDecTest, DecodeSwathPingInvalidNumBeamsAndV2Compatibility) {
  // 1. number_beams <= 0 fails with GSF_INVALID_NUM_BEAMS.
  constexpr std::int16_t kInvalidBeams[] = {0, -4};
  for (std::int16_t beams : kInvalidBeams) {
    ScopedDecodeTable dec;
    std::vector<unsigned char> raw;
    AppendPingHeaderV3(raw, beams);

    gsfSwathBathyPing ping;
    std::memset(&ping, 0, sizeof(ping));
    EXPECT_EQ(-1,
              DecodeSwathPingPadded(&ping, raw.data(), &dec.ft, kHandle,
                                           static_cast<int>(raw.size())));
    EXPECT_EQ(GSF_INVALID_NUM_BEAMS, gsfIntError());
  }

  // 2. GSF v2 (major_version_number <= 2) populates GSF_NULL_HEIGHT/SEP/TIDE.
  {
    ScopedDecodeTable dec(/*major=*/2, /*minor=*/9);
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC);
    in.sensor_data.gsfSeaBeamSpecific.EclipseTime = 777;
    const std::vector<unsigned char> encoded =
        EncodePing(in, /*major=*/2, /*minor=*/9);
    ASSERT_FALSE(encoded.empty());

    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    EXPECT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_DOUBLE_EQ(GSF_NULL_HEIGHT, out.height);
    EXPECT_DOUBLE_EQ(GSF_NULL_SEP, out.sep);
    EXPECT_DOUBLE_EQ(GSF_NULL_TIDE_CORRECTOR, out.gps_tide_corrector);
    EXPECT_EQ(777, out.sensor_data.gsfSeaBeamSpecific.EclipseTime);
  }
}

TEST(GsfDecTest, DecodeScaleFactorsIndexLookupAndValidationErrors) {
  // 1. Scale factor index lookup populates last_scale_factor_index.
  {
    ScopedDecodeTable dec;
    std::array<INDEX_REC, 2> scale_addrs = {{
        {.sec = 1700000000, .nsec = 0, .addr = 1024},
        {.sec = 1700000100, .nsec = 0, .addr = 4096},
    }};
    dec.ft.index_data.scale_factor_addr = scale_addrs.data();
    dec.ft.index_data.number_of_records[0] = 2;
    dec.ft.previous_record = 4096;

    std::vector<double> depth = {100.5, 101.25};
    // Use a 52-byte Reson 8101 subrecord after depth so gsf_dec.c:867's
    // lookahead (num_beams * 4 + 4 bytes) stays within encoded bounds.
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                     GSF_FIELD_SIZE_DEFAULT | GSF_DISABLE_COMPRESSION, 0.01);
    in.depth = depth.data();
    in.sensor_data.gsfReson8100Specific.latency = 12;

    const std::vector<unsigned char> encoded = EncodePing(in);
    ASSERT_FALSE(encoded.empty());

    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    EXPECT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(1, dec.ft.index_data.last_scale_factor_index);
    EXPECT_EQ(1, dec.ft.scales_read);
  }

  // 2. Validation error branches in DecodeScaleFactors.
  struct ScaleFactorErrorCase {
    std::uint32_t num_subrecords;
    std::uint8_t subrecord_id;
    std::uint32_t multiplier;
    int expected_error;
  };
  constexpr ScaleFactorErrorCase kCases[] = {
      {.num_subrecords = 0,
       .subrecord_id = 1,
       .multiplier = 100,
       .expected_error = GSF_TOO_MANY_ARRAY_SUBRECORDS},
      {.num_subrecords = GSF_MAX_PING_ARRAY_SUBRECORDS + 1,
       .subrecord_id = 1,
       .multiplier = 100,
       .expected_error = GSF_TOO_MANY_ARRAY_SUBRECORDS},
      {.num_subrecords = 1,
       .subrecord_id = 0,
       .multiplier = 100,
       .expected_error = GSF_UNRECOGNIZED_ARRAY_SUBRECORD_ID},
      {.num_subrecords = 1,
       .subrecord_id = GSF_MAX_PING_ARRAY_SUBRECORDS + 1,
       .multiplier = 100,
       .expected_error = GSF_UNRECOGNIZED_ARRAY_SUBRECORD_ID},
      {.num_subrecords = 1,
       .subrecord_id = GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
       .multiplier = 0,
       .expected_error = GSF_CANNOT_REPRESENT_PRECISION},
  };

  for (const auto& tc : kCases) {
    ScopedDecodeTable dec;
    std::vector<unsigned char> raw;
    AppendPingHeaderV3(raw, /*num_beams=*/2);
    const unsigned int sf_bytes = 4u + 12u * (tc.num_subrecords > 0 ? 1u : 0u);
    AppendSubrecordHeader(raw, GSF_SWATH_BATHY_SUBRECORD_SCALE_FACTORS,
                          sf_bytes);
    AppendU32Be(raw, tc.num_subrecords);
    if (tc.num_subrecords > 0) {
      AppendU32Be(raw, static_cast<std::uint32_t>(tc.subrecord_id) << 24);
      AppendU32Be(raw, tc.multiplier);
      AppendI32Be(raw, 0);
    }

    gsfSwathBathyPing ping;
    std::memset(&ping, 0, sizeof(ping));
    EXPECT_EQ(-1,
              DecodeSwathPingPadded(&ping, raw.data(), &dec.ft, kHandle,
                                           static_cast<int>(raw.size())));
    EXPECT_EQ(tc.expected_error, gsfIntError());
  }
}

TEST(GsfDecTest, DecodeSwathPingAllUncompressedArraysAndRealloc) {
  ScopedDecodeTable dec;

  auto run_all_arrays = [&](short n_beams) {
    std::vector<double> depth(n_beams);
    std::vector<double> nominal_depth(n_beams);
    std::vector<double> across_track(n_beams);
    std::vector<double> along_track(n_beams);
    std::vector<double> travel_time(n_beams);
    std::vector<double> beam_angle(n_beams);
    std::vector<double> mc_amplitude(n_beams);
    std::vector<double> mr_amplitude(n_beams);
    std::vector<double> echo_width(n_beams);
    std::vector<double> quality_factor(n_beams);
    std::vector<double> receive_heave(n_beams);
    std::vector<double> depth_error(n_beams);
    std::vector<double> across_track_error(n_beams);
    std::vector<double> along_track_error(n_beams);
    std::vector<unsigned char> quality_flags(n_beams);
    std::vector<unsigned char> beam_flags(n_beams);
    std::vector<double> signal_to_noise(n_beams);
    std::vector<double> beam_angle_forward(n_beams);
    std::vector<double> vertical_error(n_beams);
    std::vector<double> horizontal_error(n_beams);
    std::vector<unsigned short> sector_number(n_beams);
    std::vector<unsigned short> detection_info(n_beams);
    std::vector<double> incident_beam_adj(n_beams);
    std::vector<unsigned short> system_cleaning(n_beams);
    std::vector<double> doppler_corr(n_beams);
    std::vector<double> sonar_vert_uncert(n_beams);
    std::vector<double> sonar_horz_uncert(n_beams);
    std::vector<double> detection_window(n_beams);
    std::vector<double> mean_abs_coeff(n_beams);
    std::vector<double> tvg_db(n_beams);

    for (short i = 0; i < n_beams; ++i) {
      const double f = static_cast<double>(i);
      depth[i] = 100.0 + f * 1.25;
      nominal_depth[i] = 99.5 + f * 1.25;
      across_track[i] = -30.0 + f * 10.5;
      along_track[i] = -5.0 + f * 2.25;
      travel_time[i] = 0.13 + f * 0.002;
      beam_angle[i] = -45.0 + f * 12.5;
      // Keep 1-byte EncodeSignedByteArray values in 0..127 so (char) dtemp
      // does not trigger float-cast-overflow under -funsigned-char UBSan.
      mc_amplitude[i] = 20.0 + f * 3.0;
      mr_amplitude[i] = 10.0 + f * 5.0;
      echo_width[i] = 1.0 + f * 0.5;
      quality_factor[i] = 2.0 + f;
      receive_heave[i] = 4.0 + f;
      depth_error[i] = 0.1 + f * 0.02;
      across_track_error[i] = 0.2 + f * 0.03;
      along_track_error[i] = 0.3 + f * 0.04;
      quality_flags[i] = static_cast<unsigned char>(i % 4);
      beam_flags[i] = static_cast<unsigned char>(
          i == 0 ? GSF_IGNORE_BEAM : GSF_BEAM_USER_FLAG_01);
      signal_to_noise[i] = 10.0 + f * 2.0;  // 0..127 for signed/unsigned match
      beam_angle_forward[i] = 1.0 + f * 0.5;
      vertical_error[i] = 0.15 + f * 0.01;
      horizontal_error[i] = 0.25 + f * 0.02;
      sector_number[i] = static_cast<unsigned short>(i % 3);
      detection_info[i] = static_cast<unsigned short>(1 + (i % 4));
      incident_beam_adj[i] = 3.0 + f;
      system_cleaning[i] = static_cast<unsigned short>(10 + i);
      doppler_corr[i] = 2.0 + f;
      sonar_vert_uncert[i] = 0.08 + f * 0.01;
      sonar_horz_uncert[i] = 0.16 + f * 0.02;
      detection_window[i] = 0.005 + f * 0.001;
      mean_abs_coeff[i] = 15.0 + f * 0.5;
      tvg_db[i] = 12.0 + f * 0.25;
    }

    gsfSwathBathyPing in =
        MakeBasePing(n_beams, GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC);
    in.sensor_data.gsfSeaBeamSpecific.EclipseTime = 321;

    constexpr char kDefault = GSF_FIELD_SIZE_DEFAULT | GSF_DISABLE_COMPRESSION;
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY, kDefault, 0.01);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_NOMINAL_DEPTH_ARRAY,
                     kDefault, 0.01);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY, kDefault,
                     0.01);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_ALONG_TRACK_ARRAY, kDefault,
                     0.01);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_TRAVEL_TIME_ARRAY, kDefault,
                     0.0001);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_BEAM_ANGLE_ARRAY, kDefault,
                     0.01);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_MEAN_CAL_AMPLITUDE_ARRAY,
                     kDefault, 1.0);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_MEAN_REL_AMPLITUDE_ARRAY,
                     kDefault, 1.0);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_ECHO_WIDTH_ARRAY, kDefault,
                     0.5);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_QUALITY_FACTOR_ARRAY,
                     kDefault, 1.0);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_RECEIVE_HEAVE_ARRAY,
                     kDefault, 1.0);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ERROR_ARRAY, kDefault,
                     0.01);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ERROR_ARRAY,
                     kDefault, 0.01);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_ALONG_TRACK_ERROR_ARRAY,
                     kDefault, 0.01);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_SIGNAL_TO_NOISE_ARRAY,
                     kDefault, 1.0);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_BEAM_ANGLE_FORWARD_ARRAY,
                     kDefault, 0.01);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_VERTICAL_ERROR_ARRAY,
                     kDefault, 0.01);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_HORIZONTAL_ERROR_ARRAY,
                     kDefault, 0.01);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_SECTOR_NUMBER_ARRAY,
                     kDefault, 1.0);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_DETECTION_INFO_ARRAY,
                     kDefault, 1.0);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_INCIDENT_BEAM_ADJ_ARRAY,
                     kDefault, 1.0);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_SYSTEM_CLEANING_ARRAY,
                     kDefault, 1.0);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_DOPPLER_CORRECTION_ARRAY,
                     kDefault, 1.0);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_SONAR_VERT_UNCERT_ARRAY,
                     kDefault, 0.01);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_SONAR_HORZ_UNCERT_ARRAY,
                     kDefault, 0.01);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_DETECTION_WINDOW_ARRAY,
                     kDefault, 0.001);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_MEAN_ABS_COEF_ARRAY,
                     kDefault, 0.01);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_TVG_ARRAY, kDefault, 0.01);

    in.depth = depth.data();
    in.nominal_depth = nominal_depth.data();
    in.across_track = across_track.data();
    in.along_track = along_track.data();
    in.travel_time = travel_time.data();
    in.beam_angle = beam_angle.data();
    in.mc_amplitude = mc_amplitude.data();
    in.mr_amplitude = mr_amplitude.data();
    in.echo_width = echo_width.data();
    in.quality_factor = quality_factor.data();
    in.receive_heave = receive_heave.data();
    in.depth_error = depth_error.data();
    in.across_track_error = across_track_error.data();
    in.along_track_error = along_track_error.data();
    in.quality_flags = quality_flags.data();
    in.beam_flags = beam_flags.data();
    in.signal_to_noise = signal_to_noise.data();
    in.beam_angle_forward = beam_angle_forward.data();
    in.vertical_error = vertical_error.data();
    in.horizontal_error = horizontal_error.data();
    in.sector_number = sector_number.data();
    in.detection_info = detection_info.data();
    in.incident_beam_adj = incident_beam_adj.data();
    in.system_cleaning = system_cleaning.data();
    in.doppler_corr = doppler_corr.data();
    in.sonar_vert_uncert = sonar_vert_uncert.data();
    in.sonar_horz_uncert = sonar_horz_uncert.data();
    in.detection_window = detection_window.data();
    in.mean_abs_coeff = mean_abs_coeff.data();
    in.TVG_dB = tvg_db.data();

    const std::vector<unsigned char> encoded = EncodePing(in);
    ASSERT_FALSE(encoded.empty());

    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    EXPECT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    ASSERT_EQ(n_beams, out.number_beams);
    for (short i = 0; i < n_beams; ++i) {
      EXPECT_NEAR(depth[i], out.depth[i], 1e-2);
      EXPECT_NEAR(nominal_depth[i], out.nominal_depth[i], 1e-2);
      EXPECT_NEAR(across_track[i], out.across_track[i], 1e-2);
      EXPECT_NEAR(along_track[i], out.along_track[i], 1e-2);
      EXPECT_NEAR(travel_time[i], out.travel_time[i], 1e-4);
      EXPECT_NEAR(beam_angle[i], out.beam_angle[i], 1e-2);
      EXPECT_NEAR(mc_amplitude[i], out.mc_amplitude[i], 1e-6);
      EXPECT_NEAR(mr_amplitude[i], out.mr_amplitude[i], 1e-6);
      EXPECT_NEAR(echo_width[i], out.echo_width[i], 1e-6);
      EXPECT_NEAR(quality_factor[i], out.quality_factor[i], 1e-6);
      EXPECT_NEAR(receive_heave[i], out.receive_heave[i], 1e-6);
      EXPECT_NEAR(depth_error[i], out.depth_error[i], 1e-2);
      EXPECT_NEAR(across_track_error[i], out.across_track_error[i], 1e-2);
      EXPECT_NEAR(along_track_error[i], out.along_track_error[i], 1e-2);
      EXPECT_EQ(quality_flags[i], out.quality_flags[i]);
      EXPECT_EQ(beam_flags[i], out.beam_flags[i]);
      EXPECT_NEAR(signal_to_noise[i], out.signal_to_noise[i], 1e-6);
      EXPECT_NEAR(beam_angle_forward[i], out.beam_angle_forward[i], 1e-2);
      EXPECT_NEAR(vertical_error[i], out.vertical_error[i], 1e-2);
      EXPECT_NEAR(horizontal_error[i], out.horizontal_error[i], 1e-2);
      EXPECT_EQ(sector_number[i], out.sector_number[i]);
      EXPECT_EQ(detection_info[i], out.detection_info[i]);
      EXPECT_NEAR(incident_beam_adj[i], out.incident_beam_adj[i], 1e-6);
      EXPECT_EQ(system_cleaning[i], out.system_cleaning[i]);
      EXPECT_NEAR(doppler_corr[i], out.doppler_corr[i], 1e-6);
      EXPECT_NEAR(sonar_vert_uncert[i], out.sonar_vert_uncert[i], 1e-2);
      EXPECT_NEAR(sonar_horz_uncert[i], out.sonar_horz_uncert[i], 1e-2);
      EXPECT_NEAR(detection_window[i], out.detection_window[i], 1e-3);
      EXPECT_NEAR(mean_abs_coeff[i], out.mean_abs_coeff[i], 1e-2);
      EXPECT_NEAR(tvg_db[i], out.TVG_dB[i], 1e-2);
    }
  };

  // 1. 5 beams (not divisible by 4 for quality_flags padding byte), then grow
  //    to 8 beams (divisible by 4, triggers reallocation for every array).
  run_all_arrays(/*n_beams=*/5);
  run_all_arrays(/*n_beams=*/8);
}

TEST(GsfDecTest, DecodeSwathPingAlternateFieldSizesAndFourByteRealloc) {
  // 1. 4-byte unsigned/signed arrays (with growth from 3 to 6 beams) and
  //    2-byte amplitude/echo-width arrays. Brace-scoped so `dec` releases
  //    kHandle's `arraySize` row before `one_dec` opens below.
  {
    ScopedDecodeTable dec;
    auto run_four_and_two = [&](short n_beams) {
      std::vector<double> depth(n_beams);
      std::vector<double> nominal_depth(n_beams);
      std::vector<double> across_track(n_beams);
      std::vector<double> along_track(n_beams);
      std::vector<double> travel_time(n_beams);
      std::vector<double> mc_amp(n_beams);
      std::vector<double> mr_amp(n_beams);
      std::vector<double> echo_width(n_beams);
      std::vector<double> det_window(n_beams);
      std::vector<double> mean_abs(n_beams);
      std::vector<double> tvg_db(n_beams);

      for (short i = 0; i < n_beams; ++i) {
        const double f = static_cast<double>(i);
        depth[i] = 1234.567 + f * 2.5;
        nominal_depth[i] = 1230.125 + f * 2.5;
        across_track[i] = -2500.75 + f * 100.25;
        along_track[i] = -450.5 + f * 15.125;
        travel_time[i] = 1.654321 + f * 0.01;
        mc_amp[i] = -35.25 + f * 2.5;
        mr_amp[i] = 300.5 + f * 12.25;
        echo_width[i] = 0.025 + f * 0.005;
        det_window[i] = 0.12345 + f * 0.001;
        mean_abs[i] = 25.432 + f * 0.1;
        tvg_db[i] = 40.678 + f * 0.2;
      }

      gsfSwathBathyPing in =
          MakeBasePing(n_beams, GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC);
      constexpr char kFour = GSF_FIELD_SIZE_FOUR | GSF_DISABLE_COMPRESSION;
      constexpr char kTwo = GSF_FIELD_SIZE_TWO | GSF_DISABLE_COMPRESSION;
      LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY, kFour, 0.001);
      LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_NOMINAL_DEPTH_ARRAY, kFour,
                       0.001);
      LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY, kFour,
                       0.01);
      LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_ALONG_TRACK_ARRAY, kFour,
                       0.001);
      LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_TRAVEL_TIME_ARRAY, kFour,
                       0.000001);
      LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_MEAN_CAL_AMPLITUDE_ARRAY,
                       kTwo, 0.01);
      LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_MEAN_REL_AMPLITUDE_ARRAY,
                       kTwo, 0.01);
      LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_ECHO_WIDTH_ARRAY, kTwo,
                       0.001);
      LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_DETECTION_WINDOW_ARRAY,
                       kFour, 0.00001);
      LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_MEAN_ABS_COEF_ARRAY, kFour,
                       0.001);
      LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_TVG_ARRAY, kFour, 0.001);

      in.depth = depth.data();
      in.nominal_depth = nominal_depth.data();
      in.across_track = across_track.data();
      in.along_track = along_track.data();
      in.travel_time = travel_time.data();
      in.mc_amplitude = mc_amp.data();
      in.mr_amplitude = mr_amp.data();
      in.echo_width = echo_width.data();
      in.detection_window = det_window.data();
      in.mean_abs_coeff = mean_abs.data();
      in.TVG_dB = tvg_db.data();

      const std::vector<unsigned char> encoded = EncodePing(in);
      ASSERT_FALSE(encoded.empty());

      gsfSwathBathyPing out;
      std::memset(&out, 0, sizeof(out));
      EXPECT_EQ(
          static_cast<int>(encoded.size()),
          DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                static_cast<int>(encoded.size())));
      for (short i = 0; i < n_beams; ++i) {
        EXPECT_NEAR(depth[i], out.depth[i], 1e-3);
        EXPECT_NEAR(nominal_depth[i], out.nominal_depth[i], 1e-3);
        EXPECT_NEAR(across_track[i], out.across_track[i], 1e-2);
        EXPECT_NEAR(along_track[i], out.along_track[i], 1e-3);
        EXPECT_NEAR(travel_time[i], out.travel_time[i], 1e-6);
        EXPECT_NEAR(mc_amp[i], out.mc_amplitude[i], 1e-2);
        EXPECT_NEAR(mr_amp[i], out.mr_amplitude[i], 1e-2);
        EXPECT_NEAR(echo_width[i], out.echo_width[i], 1e-3);
        EXPECT_NEAR(det_window[i], out.detection_window[i], 1e-5);
        EXPECT_NEAR(mean_abs[i], out.mean_abs_coeff[i], 1e-3);
        EXPECT_NEAR(tvg_db[i], out.TVG_dB[i], 1e-3);
      }
    };

    run_four_and_two(/*n_beams=*/3);
    run_four_and_two(/*n_beams=*/6);
  }

  // 2. 1-byte field size for detection_window, mean_abs_coeff, and TVG_dB.
  {
    ScopedDecodeTable one_dec;
    std::array<double, 4> det_window = {1.0, 2.0, 3.0, 4.0};
    std::array<double, 4> mean_abs = {10.0, 12.0, 14.0, 16.0};
    std::array<double, 4> tvg_db = {20.0, 25.0, 30.0, 35.0};

    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/4, GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC);
    constexpr char kOne = GSF_FIELD_SIZE_ONE | GSF_DISABLE_COMPRESSION;
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_DETECTION_WINDOW_ARRAY, kOne,
                     1.0);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_MEAN_ABS_COEF_ARRAY, kOne,
                     1.0);
    LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_TVG_ARRAY, kOne, 1.0);
    in.detection_window = det_window.data();
    in.mean_abs_coeff = mean_abs.data();
    in.TVG_dB = tvg_db.data();

    const std::vector<unsigned char> encoded = EncodePing(in);
    ASSERT_FALSE(encoded.empty());

    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    EXPECT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &one_dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    for (int i = 0; i < 4; ++i) {
      EXPECT_NEAR(det_window[i], out.detection_window[i], 1e-6);
      EXPECT_NEAR(mean_abs[i], out.mean_abs_coeff[i], 1e-6);
      EXPECT_NEAR(tvg_db[i], out.TVG_dB[i], 1e-6);
    }
  }
}

TEST(GsfDecTest, DecodeSwathPingAllCompressedArrays) {
  ScopedDecodeTable dec;
  constexpr short kBeams = 8;

  std::array<double, kBeams> depth{};
  std::array<double, kBeams> nominal_depth{};
  std::array<double, kBeams> across_track{};
  std::array<double, kBeams> along_track{};
  std::array<double, kBeams> travel_time{};
  std::array<double, kBeams> beam_angle{};
  std::array<double, kBeams> mc_amplitude{};
  std::array<double, kBeams> mr_amplitude{};
  std::array<double, kBeams> echo_width{};
  std::array<double, kBeams> quality_factor{};
  std::array<double, kBeams> receive_heave{};
  std::array<double, kBeams> depth_error{};
  std::array<double, kBeams> across_track_error{};
  std::array<double, kBeams> along_track_error{};
  std::array<double, kBeams> signal_to_noise{};
  std::array<double, kBeams> beam_angle_forward{};
  std::array<double, kBeams> vertical_error{};
  std::array<double, kBeams> horizontal_error{};
  std::array<unsigned short, kBeams> sector_number{};
  std::array<unsigned short, kBeams> detection_info{};
  std::array<double, kBeams> incident_beam_adj{};
  std::array<unsigned short, kBeams> system_cleaning{};
  std::array<double, kBeams> doppler_corr{};
  std::array<double, kBeams> sonar_vert_uncert{};
  std::array<double, kBeams> sonar_horz_uncert{};
  std::array<double, kBeams> detection_window{};
  std::array<double, kBeams> mean_abs_coeff{};
  std::array<double, kBeams> tvg_db{};

  for (short i = 0; i < kBeams; ++i) {
    const double f = static_cast<double>(i);
    depth[i] = 200.0 + f * 0.5;
    nominal_depth[i] = 199.0 + f * 0.5;
    across_track[i] = -40.0 + f * 11.0;
    along_track[i] = -3.0 + f * 1.0;
    travel_time[i] = 0.25 + f * 0.01;
    beam_angle[i] = -35.0 + f * 10.0;
    mc_amplitude[i] = -25.0 + f * 2.0;
    mr_amplitude[i] = 15.0 + f * 3.0;
    echo_width[i] = 1.0 + f * 0.25;
    quality_factor[i] = 1.0 + f;
    receive_heave[i] = -2.0 + f * 0.5;
    depth_error[i] = 0.2 + f * 0.05;
    across_track_error[i] = 0.3 + f * 0.05;
    along_track_error[i] = 0.4 + f * 0.05;
    signal_to_noise[i] = 12.0 + f;
    beam_angle_forward[i] = 0.5 + f * 0.25;
    vertical_error[i] = 0.1 + f * 0.02;
    horizontal_error[i] = 0.2 + f * 0.03;
    sector_number[i] = static_cast<unsigned short>(i % 3);
    detection_info[i] = static_cast<unsigned short>(1 + (i % 4));
    incident_beam_adj[i] = -1.5 + f * 0.5;
    system_cleaning[i] = static_cast<unsigned short>(20 + i);
    doppler_corr[i] = -1.0 + f * 0.5;
    sonar_vert_uncert[i] = 0.05 + f * 0.01;
    sonar_horz_uncert[i] = 0.10 + f * 0.02;
    detection_window[i] = 0.01 + f * 0.002;
    mean_abs_coeff[i] = 18.0 + f * 0.25;
    tvg_db[i] = 15.0 + f * 0.5;
  }

  gsfSwathBathyPing in =
      MakeBasePing(kBeams, GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC);
  constexpr char kCompressed = GSF_FIELD_SIZE_DEFAULT | GSF_ENABLE_COMPRESSION;
  for (unsigned int id = GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY;
       id <= GSF_SWATH_BATHY_SUBRECORD_TVG_ARRAY; ++id) {
    if (id == GSF_SWATH_BATHY_SUBRECORD_QUALITY_FLAGS_ARRAY ||
        id == GSF_SWATH_BATHY_SUBRECORD_BEAM_FLAGS_ARRAY ||
        id == GSF_SWATH_BATHY_SUBRECORD_INTENSITY_SERIES_ARRAY) {
      continue;
    }
    const double prec = (id == GSF_SWATH_BATHY_SUBRECORD_TRAVEL_TIME_ARRAY ||
                         id == GSF_SWATH_BATHY_SUBRECORD_DETECTION_WINDOW_ARRAY)
                            ? 0.001
                            : 0.01;
    LoadScaledFactor(in, id, kCompressed, prec);
  }
  LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_SECTOR_NUMBER_ARRAY,
                   kCompressed, 1.0);
  LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_DETECTION_INFO_ARRAY,
                   kCompressed, 1.0);
  LoadScaledFactor(in, GSF_SWATH_BATHY_SUBRECORD_SYSTEM_CLEANING_ARRAY,
                   kCompressed, 1.0);

  in.depth = depth.data();
  in.nominal_depth = nominal_depth.data();
  in.across_track = across_track.data();
  in.along_track = along_track.data();
  in.travel_time = travel_time.data();
  in.beam_angle = beam_angle.data();
  in.mc_amplitude = mc_amplitude.data();
  in.mr_amplitude = mr_amplitude.data();
  in.echo_width = echo_width.data();
  in.quality_factor = quality_factor.data();
  in.receive_heave = receive_heave.data();
  in.depth_error = depth_error.data();
  in.across_track_error = across_track_error.data();
  in.along_track_error = along_track_error.data();
  in.signal_to_noise = signal_to_noise.data();
  in.beam_angle_forward = beam_angle_forward.data();
  in.vertical_error = vertical_error.data();
  in.horizontal_error = horizontal_error.data();
  in.sector_number = sector_number.data();
  in.detection_info = detection_info.data();
  in.incident_beam_adj = incident_beam_adj.data();
  in.system_cleaning = system_cleaning.data();
  in.doppler_corr = doppler_corr.data();
  in.sonar_vert_uncert = sonar_vert_uncert.data();
  in.sonar_horz_uncert = sonar_horz_uncert.data();
  in.detection_window = detection_window.data();
  in.mean_abs_coeff = mean_abs_coeff.data();
  in.TVG_dB = tvg_db.data();

  const std::vector<unsigned char> encoded = EncodePing(in);
  ASSERT_FALSE(encoded.empty());

  gsfSwathBathyPing out;
  std::memset(&out, 0, sizeof(out));
  EXPECT_EQ(static_cast<int>(encoded.size()),
            DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                         static_cast<int>(encoded.size())));
  for (short i = 0; i < kBeams; ++i) {
    EXPECT_NEAR(depth[i], out.depth[i], 1e-2);
    EXPECT_NEAR(nominal_depth[i], out.nominal_depth[i], 1e-2);
    EXPECT_NEAR(across_track[i], out.across_track[i], 1e-2);
    EXPECT_NEAR(along_track[i], out.along_track[i], 1e-2);
    EXPECT_NEAR(travel_time[i], out.travel_time[i], 1e-3);
    EXPECT_NEAR(beam_angle[i], out.beam_angle[i], 1e-2);
    EXPECT_NEAR(mc_amplitude[i], out.mc_amplitude[i], 1e-2);
    EXPECT_NEAR(mr_amplitude[i], out.mr_amplitude[i], 1e-2);
    EXPECT_NEAR(echo_width[i], out.echo_width[i], 1e-2);
    EXPECT_NEAR(quality_factor[i], out.quality_factor[i], 1e-2);
    EXPECT_NEAR(receive_heave[i], out.receive_heave[i], 1e-2);
    EXPECT_NEAR(depth_error[i], out.depth_error[i], 1e-2);
    EXPECT_NEAR(across_track_error[i], out.across_track_error[i], 1e-2);
    EXPECT_NEAR(along_track_error[i], out.along_track_error[i], 1e-2);
    EXPECT_NEAR(signal_to_noise[i], out.signal_to_noise[i], 1e-2);
    EXPECT_NEAR(beam_angle_forward[i], out.beam_angle_forward[i], 1e-2);
    EXPECT_NEAR(vertical_error[i], out.vertical_error[i], 1e-2);
    EXPECT_NEAR(horizontal_error[i], out.horizontal_error[i], 1e-2);
    EXPECT_EQ(sector_number[i], out.sector_number[i]);
    EXPECT_EQ(detection_info[i], out.detection_info[i]);
    EXPECT_NEAR(incident_beam_adj[i], out.incident_beam_adj[i], 1e-2);
    EXPECT_EQ(system_cleaning[i], out.system_cleaning[i]);
    EXPECT_NEAR(doppler_corr[i], out.doppler_corr[i], 1e-2);
    EXPECT_NEAR(sonar_vert_uncert[i], out.sonar_vert_uncert[i], 1e-2);
    EXPECT_NEAR(sonar_horz_uncert[i], out.sonar_horz_uncert[i], 1e-2);
    EXPECT_NEAR(detection_window[i], out.detection_window[i], 1e-3);
    EXPECT_NEAR(mean_abs_coeff[i], out.mean_abs_coeff[i], 1e-2);
    EXPECT_NEAR(tvg_db[i], out.TVG_dB[i], 1e-2);
  }
}

TEST(GsfDecTest, DecodeSwathPingFieldSizeResyncAndQualityFlagsEdges) {
  // 1. Field-size resynchronization loop recovering 1-byte, 2-byte, and
  //    4-byte field sizes from a mismatched subrecord_size (3 bytes).
  constexpr unsigned int kArrayPayloadBytes[] = {4, 8, 16};
  for (unsigned int actual_bytes : kArrayPayloadBytes) {
    ScopedDecodeTable dec;
    std::vector<unsigned char> raw;
    AppendPingHeaderV3(raw, /*num_beams=*/4);

    // Scale factors for DETECTION_WINDOW_ARRAY (29) and ACROSS_TRACK_ARRAY (2).
    AppendSubrecordHeader(raw, GSF_SWATH_BATHY_SUBRECORD_SCALE_FACTORS,
                          4 + 12 * 2);
    AppendU32Be(raw, 2u);
    AppendU32Be(raw, (GSF_SWATH_BATHY_SUBRECORD_DETECTION_WINDOW_ARRAY << 24) |
                         (GSF_FIELD_SIZE_DEFAULT << 16));
    AppendU32Be(raw, 1u);
    AppendI32Be(raw, 0);
    AppendU32Be(raw, (GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY << 24) |
                         (GSF_FIELD_SIZE_TWO << 16));
    AppendU32Be(raw, 100u);
    AppendI32Be(raw, 0);

    // Claim size 3 (not divisible by 4) while placing actual_bytes of data
    // before the valid ACROSS_TRACK_ARRAY subrecord (next_size == 2 * 4 == 8).
    AppendSubrecordHeader(raw, GSF_SWATH_BATHY_SUBRECORD_DETECTION_WINDOW_ARRAY,
                          3u);
    if (actual_bytes == 4) {
      for (unsigned int i = 0; i < 4; ++i) {
        AppendU8(raw, static_cast<std::uint8_t>(10 + i));
      }
    } else if (actual_bytes == 8) {
      for (unsigned int i = 0; i < 4; ++i) {
        AppendU16Be(raw, static_cast<std::uint16_t>(100 + i));
      }
    } else {
      for (unsigned int i = 0; i < 4; ++i) {
        AppendU32Be(raw, static_cast<std::uint32_t>(1000 + i));
      }
    }

    AppendSubrecordHeader(raw, GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY,
                          8u);
    for (int i = 0; i < 4; ++i) {
      AppendI16Be(raw, static_cast<std::int16_t>((i - 1) * 100));
    }

    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    EXPECT_EQ(static_cast<int>(raw.size()),
              DecodeSwathPingPadded(&out, raw.data(), &dec.ft, kHandle,
                                    static_cast<int>(raw.size())));
    ASSERT_THAT(out.detection_window, NotNull());
    EXPECT_DOUBLE_EQ(
        actual_bytes == 4 ? 10.0 : (actual_bytes == 8 ? 100.0 : 1000.0),
        out.detection_window[0]);
    ASSERT_THAT(out.across_track, NotNull());
    EXPECT_NEAR(-1.0, out.across_track[0], 1e-2);
    EXPECT_NEAR(2.0, out.across_track[3], 1e-2);
  }

  // 2. Quality flags with ft.update_flag == 1 and truncated sr_size*4 < beams
  //    must fail with GSF_QUALITY_FLAGS_DECODE_ERROR; with update_flag == 0 it
  //    decodes only sr_size*4 entries. Brace-scoped so `dec_update` dies before
  //    `dec_readonly` reuses `kHandle`.
  {
    std::vector<unsigned char> raw;
    AppendPingHeaderV3(raw, /*num_beams=*/5);
    AppendSubrecordHeader(raw, GSF_SWATH_BATHY_SUBRECORD_QUALITY_FLAGS_ARRAY,
                          1u);
    AppendU8(raw, 0xE4u);  // beams 0..3 -> 3, 2, 1, 0

    {
      ScopedDecodeTable dec_update;
      dec_update.ft.update_flag = 1;
      gsfSwathBathyPing fail_ping;
      std::memset(&fail_ping, 0, sizeof(fail_ping));
      EXPECT_EQ(-1, DecodeSwathPingPadded(&fail_ping, raw.data(),
                                          &dec_update.ft, kHandle,
                                          static_cast<int>(raw.size())));
      EXPECT_EQ(GSF_QUALITY_FLAGS_DECODE_ERROR, gsfIntError());
    }

    ScopedDecodeTable dec_readonly;
    gsfSwathBathyPing ok_ping;
    std::memset(&ok_ping, 0, sizeof(ok_ping));
    EXPECT_EQ(
        static_cast<int>(raw.size()),
        DecodeSwathPingPadded(&ok_ping, raw.data(), &dec_readonly.ft,
                              kHandle, static_cast<int>(raw.size())));
    ASSERT_THAT(ok_ping.quality_flags, NotNull());
    EXPECT_THAT(std::vector<unsigned char>(ok_ping.quality_flags,
                                           ok_ping.quality_flags + 5),
                ElementsAre(3, 2, 1, 0, 0));
  }
}

TEST(GsfDecTest, DecodeSwathPingMissingScaleFactorErrorsAndMidPingCleanup) {
  struct MissingScaleCase {
    unsigned int subrecord_id;
    unsigned int bytes_per_beam;
    bool compressed;
  };
  constexpr MissingScaleCase kCases[] = {
      {.subrecord_id = GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
       .bytes_per_beam = 2,
       .compressed = false},
      {.subrecord_id = GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
       .bytes_per_beam = 4,
       .compressed = false},
      {.subrecord_id = GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
       .bytes_per_beam = 2,
       .compressed = true},
      {.subrecord_id = GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY,
       .bytes_per_beam = 2,
       .compressed = false},
      {.subrecord_id = GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY,
       .bytes_per_beam = 4,
       .compressed = false},
      {.subrecord_id = GSF_SWATH_BATHY_SUBRECORD_QUALITY_FACTOR_ARRAY,
       .bytes_per_beam = 1,
       .compressed = false},
      {.subrecord_id = GSF_SWATH_BATHY_SUBRECORD_RECEIVE_HEAVE_ARRAY,
       .bytes_per_beam = 1,
       .compressed = false},
      {.subrecord_id = GSF_SWATH_BATHY_SUBRECORD_SECTOR_NUMBER_ARRAY,
       .bytes_per_beam = 1,
       .compressed = false},
  };

  constexpr short kBeams = 2;
  for (const auto& tc : kCases) {
    ScopedDecodeTable dec;
    if (tc.compressed) {
      dec.ft.rec.mb_ping.scaleFactors.scaleTable[tc.subrecord_id - 1]
          .compressionFlag = GSF_ENABLE_COMPRESSION;
    }
    std::vector<unsigned char> raw;
    AppendPingHeaderV3(raw, kBeams);
    const unsigned int payload =
        static_cast<unsigned int>(kBeams) * tc.bytes_per_beam;
    AppendSubrecordHeader(raw, tc.subrecord_id, payload);
    raw.insert(raw.end(), payload, 0);

    gsfSwathBathyPing ping;
    std::memset(&ping, 0, sizeof(ping));
    EXPECT_EQ(-1,
              DecodeSwathPingPadded(&ping, raw.data(), &dec.ft, kHandle,
                                           static_cast<int>(raw.size())));
    EXPECT_EQ(GSF_ILLEGAL_SCALE_FACTOR_MULTIPLIER, gsfIntError());
  }

  // Mid-ping failure after depth is allocated must keep ft.rec valid for
  // gsfFree(&ft.rec) cleanup without leaking depth.
  {
    ScopedDecodeTable dec;
    std::vector<unsigned char> raw;
    AppendPingHeaderV3(raw, kBeams);
    AppendSubrecordHeader(raw, GSF_SWATH_BATHY_SUBRECORD_SCALE_FACTORS, 4 + 12);
    AppendU32Be(raw, 1u);
    AppendU32Be(raw, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY << 24);
    AppendU32Be(raw, 100u);
    AppendI32Be(raw, 0);
    AppendSubrecordHeader(raw, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY, 4u);
    AppendU16Be(raw, 1000u);
    AppendU16Be(raw, 2000u);
    AppendSubrecordHeader(raw, GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY,
                          4u);
    AppendI16Be(raw, -50);
    AppendI16Be(raw, 50);

    gsfSwathBathyPing ping;
    std::memset(&ping, 0, sizeof(ping));
    EXPECT_EQ(-1,
              DecodeSwathPingPadded(&ping, raw.data(), &dec.ft, kHandle,
                                           static_cast<int>(raw.size())));
    EXPECT_EQ(GSF_ILLEGAL_SCALE_FACTOR_MULTIPLIER, gsfIntError());
    EXPECT_THAT(dec.ft.rec.mb_ping.depth, NotNull());
  }
}

TEST(GsfDecTest, DecodeSwathPingUnknownTrailingAndTruncatedSubrecords) {
  // 1. Padded UNKNOWN subrecord enters switch case
  // GSF_SWATH_BATHY_SUBRECORD_UNKNOWN.
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in =
        MakeBasePing(/*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_UNKNOWN);
    std::vector<unsigned char> encoded = EncodePing(in);
    ASSERT_FALSE(encoded.empty());
    // Append 4 bytes of padding so (record_size - bytes) > 4 triggers switch.
    encoded.insert(encoded.end(), 4, 0);

    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    out.sensor_id = -1;
    EXPECT_EQ(
        static_cast<int>(encoded.size()) - 4,
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(GSF_SWATH_BATHY_SUBRECORD_UNKNOWN, out.sensor_id);
  }

  // 2. Trailing 0-size sensor subrecord ((record_size - bytes) == 4).
  {
    ScopedDecodeTable dec;
    std::vector<unsigned char> raw;
    AppendPingHeaderV3(raw, /*num_beams=*/2);
    AppendSubrecordHeader(raw, GSF_SWATH_BATHY_SUBRECORD_SCALE_FACTORS, 4 + 12);
    AppendU32Be(raw, 1u);
    AppendU32Be(raw, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY << 24);
    AppendU32Be(raw, 100u);
    AppendI32Be(raw, 0);
    AppendSubrecordHeader(raw, GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY, 4u);
    AppendU16Be(raw, 500u);
    AppendU16Be(raw, 600u);
    AppendSubrecordHeader(raw, GSF_SWATH_BATHY_SUBRECORD_EM100_SPECIFIC, 0u);

    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    EXPECT_EQ(static_cast<int>(raw.size()),
              DecodeSwathPingPadded(&out, raw.data(), &dec.ft, kHandle,
                                           static_cast<int>(raw.size())));
    EXPECT_EQ(GSF_SWATH_BATHY_SUBRECORD_EM100_SPECIFIC, out.sensor_id);
  }

  // 3. Unknown subrecord (id=154 within ft.rec bounds) skip vs truncated fail.
  {
    ScopedDecodeTable dec;
    std::vector<unsigned char> raw;
    AppendPingHeaderV3(raw, /*num_beams=*/2);
    AppendSubrecordHeader(raw, 154, 4u);
    AppendU32Be(raw, 0xCAFEBABEu);
    AppendSubrecordHeader(raw, GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC, 2u);
    AppendU16Be(raw, 4321u);

    gsfError = GSF_NORMAL;
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    EXPECT_EQ(static_cast<int>(raw.size()),
              DecodeSwathPingPadded(&out, raw.data(), &dec.ft, kHandle,
                                           static_cast<int>(raw.size())));
    EXPECT_EQ(GSF_UNRECOGNIZED_SUBRECORD_ID, gsfIntError());
    EXPECT_EQ(GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC, out.sensor_id);
    EXPECT_EQ(4321, out.sensor_data.gsfSeaBeamSpecific.EclipseTime);
  }

  {
    ScopedDecodeTable dec;
    std::vector<unsigned char> truncated;
    AppendPingHeaderV3(truncated, /*num_beams=*/2);
    AppendSubrecordHeader(truncated, 154, 120u);
    AppendU32Be(truncated, 0x01020304u);

    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    EXPECT_EQ(-1, DecodeSwathPingPadded(
                      &out, truncated.data(), &dec.ft, kHandle,
                      static_cast<int>(truncated.size())));
    EXPECT_EQ(GSF_UNRECOGNIZED_SUBRECORD_ID, gsfIntError());
  }
}

TEST(GsfDecTest, DecodeBrbIntensityBitsPerSampleAndRealloc) {
  auto encode_and_decode =
      [](ScopedDecodeTable& dec, unsigned char bps,
         const std::vector<std::vector<unsigned int>>& beam_samples) {
        const short n_beams = static_cast<short>(beam_samples.size());
        gsfBRBIntensity brb;
        std::memset(&brb, 0, sizeof(brb));
        brb.bits_per_sample = bps;
        brb.applied_corrections =
            GSF_INTENSITY_LINEAR | GSF_INTENSITY_CALIBRATED;
        for (int i = 0; i < 16; ++i) {
          brb.spare[i] = static_cast<unsigned char>(i + 1);
        }
        std::vector<std::vector<unsigned int>> mutable_samples = beam_samples;
        std::vector<gsfTimeSeriesIntensity> ts(n_beams);
        for (short i = 0; i < n_beams; ++i) {
          ts[i].sample_count =
              static_cast<unsigned short>(mutable_samples[i].size());
          ts[i].detect_sample =
              static_cast<unsigned short>(mutable_samples[i].empty() ? 0 : 1);
          ts[i].start_range_samples = static_cast<unsigned short>(10 + i);
          ts[i].samples = mutable_samples[i].data();
        }
        brb.time_series = ts.data();

        gsfSwathBathyPing in =
            MakeBasePing(n_beams, GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC);
        in.sensor_data.gsfSeaBeamSpecific.EclipseTime = 55;
        in.brb_inten = &brb;

        const std::vector<unsigned char> encoded = EncodePing(in);
        ASSERT_FALSE(encoded.empty());

        gsfSwathBathyPing out;
        std::memset(&out, 0, sizeof(out));
        ASSERT_EQ(
            static_cast<int>(encoded.size()),
            DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                         static_cast<int>(encoded.size())));
        ASSERT_THAT(out.brb_inten, NotNull());
        EXPECT_EQ(bps, out.brb_inten->bits_per_sample);
        EXPECT_EQ(brb.applied_corrections, out.brb_inten->applied_corrections);
        EXPECT_THAT(std::vector<unsigned char>(out.brb_inten->spare,
                                               out.brb_inten->spare + 16),
                    ElementsAreArray(brb.spare));
        for (short i = 0; i < n_beams; ++i) {
          EXPECT_EQ(ts[i].sample_count,
                    out.brb_inten->time_series[i].sample_count);
          EXPECT_EQ(ts[i].detect_sample,
                    out.brb_inten->time_series[i].detect_sample);
          EXPECT_EQ(ts[i].start_range_samples,
                    out.brb_inten->time_series[i].start_range_samples);
          EXPECT_THAT(
              std::vector<unsigned int>(
                  out.brb_inten->time_series[i].samples,
                  out.brb_inten->time_series[i].samples + ts[i].sample_count),
              ElementsAreArray(beam_samples[i]));
        }
      };

  // 1. 8-bit, 12-bit (odd & even sample counts), 16-bit, and 32-bit samples.
  {
    ScopedDecodeTable dec;
    encode_and_decode(dec, 8, {{10, 200, 255}, {0, 42, 128}});
  }
  {
    ScopedDecodeTable dec;
    // Beam 0 has odd count (3), beam 1 has even count (4).
    encode_and_decode(
        dec, 12, {{0x123u, 0xABCu, 0xFEDu}, {0x001u, 0x555u, 0xFFFu, 0x0A0u}});
  }
  {
    ScopedDecodeTable dec;
    encode_and_decode(dec, 16, {{1000u, 50000u}, {65535u, 42u}});
  }
  {
    ScopedDecodeTable dec;
    encode_and_decode(dec, 32, {{0x12345678u, 0x9ABCDEF0u}, {77u, 999999u}});
  }

  // 2. Same beam count with larger sample_count triggers samples reallocation;
  //    larger beam count triggers time_series + samplesArraySize reallocation.
  {
    ScopedDecodeTable dec;
    encode_and_decode(dec, 8, {{1, 2}, {3, 4}});
    encode_and_decode(dec, 8, {{10, 20, 30, 40, 50}, {60, 70, 80, 90}});

    // Free per-beam samples manually before growing num_beams because upstream
    // DecodeBRBIntensity frees time_series without freeing inner samples.
    const int prev_beams =
        arraySize[kHandle - 1]
                 [GSF_SWATH_BATHY_SUBRECORD_INTENSITY_SERIES_ARRAY - 1];
    for (int i = 0; i < prev_beams; ++i) {
      std::free(dec.ft.rec.mb_ping.brb_inten->time_series[i].samples);
      dec.ft.rec.mb_ping.brb_inten->time_series[i].samples = nullptr;
    }
    encode_and_decode(dec, 8, {{11}, {22}, {33}, {44}});
  }
}

TEST(GsfDecTest, DecodeBrbIntensityAllSensorImageryTypes) {
  auto encode_decode_imagery = [](int sensor_id,
                                  const gsfSensorImagery& imagery,
                                  gsfSensorImagery& out_imagery) {
    ScopedDecodeTable dec;
    std::array<unsigned int, 2> s0 = {10, 20};
    std::array<unsigned int, 2> s1 = {30, 40};
    std::array<gsfTimeSeriesIntensity, 2> ts{};
    ts[0].sample_count = 2;
    ts[0].samples = s0.data();
    ts[1].sample_count = 2;
    ts[1].samples = s1.data();

    gsfBRBIntensity brb;
    std::memset(&brb, 0, sizeof(brb));
    brb.bits_per_sample = 8;
    brb.applied_corrections = GSF_INTENSITY_POWER;
    brb.sensor_imagery = imagery;
    brb.time_series = ts.data();

    gsfSwathBathyPing in = MakeBasePing(/*num_beams=*/2, sensor_id);
    in.brb_inten = &brb;
    const std::vector<unsigned char> encoded = EncodePing(in);
    ASSERT_FALSE(encoded.empty());

    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    ASSERT_THAT(out.brb_inten, NotNull());
    out_imagery = out.brb_inten->sensor_imagery;
  };

  // 1. EM3 imagery (EM3000).
  {
    gsfSensorImagery in;
    std::memset(&in, 0, sizeof(in));
    in.gsfEM3ImagerySpecific.range_norm = 120;
    in.gsfEM3ImagerySpecific.start_tvg_ramp = 10;
    in.gsfEM3ImagerySpecific.stop_tvg_ramp = 200;
    in.gsfEM3ImagerySpecific.bsn = 25;
    in.gsfEM3ImagerySpecific.bso = 35;
    in.gsfEM3ImagerySpecific.mean_absorption = 18.75;
    in.gsfEM3ImagerySpecific.offset = -100;
    in.gsfEM3ImagerySpecific.scale = 2;
    in.gsfEM3ImagerySpecific.spare[0] = 0x5A;
    gsfSensorImagery out;
    std::memset(&out, 0, sizeof(out));
    encode_decode_imagery(GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC, in, out);
    EXPECT_EQ(120, out.gsfEM3ImagerySpecific.range_norm);
    EXPECT_EQ(10, out.gsfEM3ImagerySpecific.start_tvg_ramp);
    EXPECT_EQ(200, out.gsfEM3ImagerySpecific.stop_tvg_ramp);
    EXPECT_EQ(25, out.gsfEM3ImagerySpecific.bsn);
    EXPECT_EQ(35, out.gsfEM3ImagerySpecific.bso);
    EXPECT_NEAR(18.75, out.gsfEM3ImagerySpecific.mean_absorption, 1e-2);
    EXPECT_EQ(-100, out.gsfEM3ImagerySpecific.offset);
    EXPECT_EQ(2, out.gsfEM3ImagerySpecific.scale);
    EXPECT_EQ(0x5A, out.gsfEM3ImagerySpecific.spare[0]);
  }

  // 2. Reson 7100 imagery.
  {
    gsfSensorImagery in;
    std::memset(&in, 0, sizeof(in));
    in.gsfReson7100ImagerySpecific.size = 64;
    in.gsfReson7100ImagerySpecific.spare[0] = 0x11;
    gsfSensorImagery out;
    std::memset(&out, 0, sizeof(out));
    encode_decode_imagery(GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC, in,
                          out);
    EXPECT_EQ(64, out.gsfReson7100ImagerySpecific.size);
    EXPECT_EQ(0x11, out.gsfReson7100ImagerySpecific.spare[0]);
  }

  // 3. Reson TSeries imagery.
  {
    gsfSensorImagery in;
    std::memset(&in, 0, sizeof(in));
    in.gsfResonTSeriesImagerySpecific.size = 48;
    in.gsfResonTSeriesImagerySpecific.spare[1] = 0x22;
    gsfSensorImagery out;
    std::memset(&out, 0, sizeof(out));
    encode_decode_imagery(GSF_SWATH_BATHY_SUBRECORD_RESON_TSERIES_SPECIFIC, in,
                          out);
    EXPECT_EQ(48, out.gsfResonTSeriesImagerySpecific.size);
    EXPECT_EQ(0x22, out.gsfResonTSeriesImagerySpecific.spare[1]);
  }

  // 4. Reson 8100 imagery.
  {
    gsfSensorImagery in;
    std::memset(&in, 0, sizeof(in));
    in.gsfReson8100ImagerySpecific.spare[0] = 0x33;
    in.gsfReson8100ImagerySpecific.spare[7] = 0x44;
    gsfSensorImagery out;
    std::memset(&out, 0, sizeof(out));
    encode_decode_imagery(GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC, in,
                          out);
    EXPECT_EQ(0x33, out.gsfReson8100ImagerySpecific.spare[0]);
    EXPECT_EQ(0x44, out.gsfReson8100ImagerySpecific.spare[7]);
  }

  // 5. EM4 imagery (EM710).
  {
    gsfSensorImagery in;
    std::memset(&in, 0, sizeof(in));
    in.gsfEM4ImagerySpecific.sampling_frequency = 40000.25;
    in.gsfEM4ImagerySpecific.mean_absorption = 19.5;
    in.gsfEM4ImagerySpecific.tx_pulse_length = 250.0;
    in.gsfEM4ImagerySpecific.range_norm = 300;
    in.gsfEM4ImagerySpecific.start_tvg_ramp = 15;
    in.gsfEM4ImagerySpecific.stop_tvg_ramp = 400;
    in.gsfEM4ImagerySpecific.bsn = -23.5;
    in.gsfEM4ImagerySpecific.bso = -31.2;
    in.gsfEM4ImagerySpecific.tx_beam_width = 1.4;
    in.gsfEM4ImagerySpecific.tvg_cross_over = 40.5;
    in.gsfEM4ImagerySpecific.offset = -200;
    in.gsfEM4ImagerySpecific.scale = 10;
    in.gsfEM4ImagerySpecific.spare[0] = 0x55;
    gsfSensorImagery out;
    std::memset(&out, 0, sizeof(out));
    encode_decode_imagery(GSF_SWATH_BATHY_SUBRECORD_EM710_SPECIFIC, in, out);
    EXPECT_NEAR(40000.25, out.gsfEM4ImagerySpecific.sampling_frequency, 1e-6);
    EXPECT_NEAR(19.5, out.gsfEM4ImagerySpecific.mean_absorption, 1e-2);
    EXPECT_NEAR(250.0, out.gsfEM4ImagerySpecific.tx_pulse_length, 1e-6);
    EXPECT_EQ(300, out.gsfEM4ImagerySpecific.range_norm);
    EXPECT_EQ(15, out.gsfEM4ImagerySpecific.start_tvg_ramp);
    EXPECT_EQ(400, out.gsfEM4ImagerySpecific.stop_tvg_ramp);
    EXPECT_NEAR(-23.5, out.gsfEM4ImagerySpecific.bsn, 1e-1);
    EXPECT_NEAR(-31.2, out.gsfEM4ImagerySpecific.bso, 1e-1);
    EXPECT_NEAR(1.4, out.gsfEM4ImagerySpecific.tx_beam_width, 1e-1);
    EXPECT_NEAR(40.5, out.gsfEM4ImagerySpecific.tvg_cross_over, 1e-1);
    EXPECT_EQ(-200, out.gsfEM4ImagerySpecific.offset);
    EXPECT_EQ(10, out.gsfEM4ImagerySpecific.scale);
    EXPECT_EQ(0x55, out.gsfEM4ImagerySpecific.spare[0]);
  }

  // 6. KMALL imagery.
  {
    gsfSensorImagery in;
    std::memset(&in, 0, sizeof(in));
    gsfSensorImagery out;
    std::memset(&out, 0, sizeof(out));
    out.gsfKMALLImagerySpecific.spare[0] = 0xFF;
    encode_decode_imagery(GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC, in, out);
    EXPECT_EQ(0, out.gsfKMALLImagerySpecific.spare[0]);
  }

  // 7. Klein 5410 BSS imagery.
  {
    gsfSensorImagery in;
    std::memset(&in, 0, sizeof(in));
    in.gsfKlein5410BssImagerySpecific.res_mode = 1;
    in.gsfKlein5410BssImagerySpecific.tvg_page = 3;
    in.gsfKlein5410BssImagerySpecific.beam_id[0] = 1;
    in.gsfKlein5410BssImagerySpecific.beam_id[4] = 5;
    in.gsfKlein5410BssImagerySpecific.spare[0] = 0x77;
    gsfSensorImagery out;
    std::memset(&out, 0, sizeof(out));
    encode_decode_imagery(GSF_SWATH_BATHY_SUBRECORD_KLEIN_5410_BSS_SPECIFIC, in,
                          out);
    EXPECT_EQ(1u, out.gsfKlein5410BssImagerySpecific.res_mode);
    EXPECT_EQ(3u, out.gsfKlein5410BssImagerySpecific.tvg_page);
    EXPECT_EQ(1u, out.gsfKlein5410BssImagerySpecific.beam_id[0]);
    EXPECT_EQ(5u, out.gsfKlein5410BssImagerySpecific.beam_id[4]);
    EXPECT_EQ(0x77, out.gsfKlein5410BssImagerySpecific.spare[0]);
  }

  // 8. R2Sonic imagery.
  {
    gsfSensorImagery in;
    std::memset(&in, 0, sizeof(in));
    std::copy_n("2024", 5, in.gsfR2SonicImagerySpecific.model_number);
    std::copy_n("100017", 7, in.gsfR2SonicImagerySpecific.serial_number);
    in.gsfR2SonicImagerySpecific.dg_time.tv_sec = 1700009000;
    in.gsfR2SonicImagerySpecific.dg_time.tv_nsec = 123000000;
    in.gsfR2SonicImagerySpecific.ping_number = 888;
    in.gsfR2SonicImagerySpecific.ping_period = 0.25;
    in.gsfR2SonicImagerySpecific.sound_speed = 1502.35;
    in.gsfR2SonicImagerySpecific.frequency = 400000.0;
    in.gsfR2SonicImagerySpecific.tx_power = 210.5;
    in.gsfR2SonicImagerySpecific.tx_pulse_width = 0.00015;
    in.gsfR2SonicImagerySpecific.tx_beamwidth_vert = 1.0;
    in.gsfR2SonicImagerySpecific.tx_beamwidth_horiz = 2.0;
    in.gsfR2SonicImagerySpecific.tx_steering_vert = 0.5;
    in.gsfR2SonicImagerySpecific.tx_steering_horiz = 0.75;
    in.gsfR2SonicImagerySpecific.tx_misc_info = 12;
    in.gsfR2SonicImagerySpecific.rx_bandwidth = 60000.0;
    in.gsfR2SonicImagerySpecific.rx_sample_rate = 62500.0;
    in.gsfR2SonicImagerySpecific.rx_range = 120.5;
    in.gsfR2SonicImagerySpecific.rx_gain = 18.0;
    in.gsfR2SonicImagerySpecific.rx_spreading = 20.0;
    in.gsfR2SonicImagerySpecific.rx_absorption = 35.5;
    in.gsfR2SonicImagerySpecific.rx_mount_tilt = 1.25;
    in.gsfR2SonicImagerySpecific.rx_misc_info = 34;
    in.gsfR2SonicImagerySpecific.reserved = 5;
    in.gsfR2SonicImagerySpecific.num_beams = 256;
    for (int i = 0; i < 6; ++i) {
      in.gsfR2SonicImagerySpecific.more_info[i] = 0.5 + i * 0.25;
    }
    gsfSensorImagery out;
    std::memset(&out, 0, sizeof(out));
    encode_decode_imagery(GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2024_SPECIFIC, in,
                          out);
    EXPECT_STREQ("2024", reinterpret_cast<const char*>(
                             out.gsfR2SonicImagerySpecific.model_number));
    EXPECT_STREQ("100017", reinterpret_cast<const char*>(
                               out.gsfR2SonicImagerySpecific.serial_number));
    EXPECT_EQ(888u, out.gsfR2SonicImagerySpecific.ping_number);
    EXPECT_NEAR(0.5, out.gsfR2SonicImagerySpecific.tx_steering_vert, 1e-6);
    EXPECT_NEAR(1.25, out.gsfR2SonicImagerySpecific.rx_mount_tilt, 1e-6);
    EXPECT_EQ(256, out.gsfR2SonicImagerySpecific.num_beams);
    EXPECT_NEAR(1.75, out.gsfR2SonicImagerySpecific.more_info[5], 1e-6);
  }
}

TEST(GsfDecTest, DecodeLegacyAndSinglebeamSwathSensorSubrecords) {
  // 1. SeaBeam 16-beam (102).
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC);
    in.sensor_data.gsfSeaBeamSpecific.EclipseTime = 1234;
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC, out.sensor_id);
    EXPECT_EQ(1234, out.sensor_data.gsfSeaBeamSpecific.EclipseTime);
  }

  // 2. EM12 (103).
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in =
        MakeBasePing(/*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_EM12_SPECIFIC);
    in.sensor_data.gsfEM12Specific.ping_number = 456;
    in.sensor_data.gsfEM12Specific.resolution = 1;
    in.sensor_data.gsfEM12Specific.ping_quality = 81;
    in.sensor_data.gsfEM12Specific.sound_velocity = 1502.4;
    in.sensor_data.gsfEM12Specific.mode = 4;
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(GSF_SWATH_BATHY_SUBRECORD_EM12_SPECIFIC, out.sensor_id);
    EXPECT_EQ(456, out.sensor_data.gsfEM12Specific.ping_number);
    EXPECT_EQ(1, out.sensor_data.gsfEM12Specific.resolution);
    EXPECT_EQ(81, out.sensor_data.gsfEM12Specific.ping_quality);
    EXPECT_NEAR(1502.4, out.sensor_data.gsfEM12Specific.sound_velocity, 1e-1);
    EXPECT_EQ(4, out.sensor_data.gsfEM12Specific.mode);
  }

  // 3. EM100 (104): hand-crafted raw subrecord bytes.
  {
    ScopedDecodeTable dec;
    std::vector<unsigned char> raw;
    AppendPingHeaderV3(raw, /*num_beams=*/2);
    AppendSubrecordHeader(raw, GSF_SWATH_BATHY_SUBRECORD_EM100_SPECIFIC, 11u);
    AppendI16Be(raw, -250);   // ship_pitch (-2.50)
    AppendI16Be(raw, 125);    // transducer_pitch (1.25)
    AppendU8(raw, 3);         // mode
    AppendU8(raw, 5);         // power
    AppendU8(raw, 12);        // attenuation
    AppendU8(raw, 7);         // tvg
    AppendU8(raw, 4);         // pulse_length
    AppendU16Be(raw, 1234u);  // counter

    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(static_cast<int>(raw.size()),
              DecodeSwathPingPadded(&out, raw.data(), &dec.ft, kHandle,
                                           static_cast<int>(raw.size())));
    EXPECT_EQ(GSF_SWATH_BATHY_SUBRECORD_EM100_SPECIFIC, out.sensor_id);
    EXPECT_NEAR(-2.50, out.sensor_data.gsfEM100Specific.ship_pitch, 1e-2);
    EXPECT_NEAR(1.25, out.sensor_data.gsfEM100Specific.transducer_pitch, 1e-2);
    EXPECT_EQ(3, out.sensor_data.gsfEM100Specific.mode);
    EXPECT_EQ(5, out.sensor_data.gsfEM100Specific.power);
    EXPECT_EQ(12, out.sensor_data.gsfEM100Specific.attenuation);
    EXPECT_EQ(7, out.sensor_data.gsfEM100Specific.tvg);
    EXPECT_EQ(4, out.sensor_data.gsfEM100Specific.pulse_length);
    EXPECT_EQ(1234, out.sensor_data.gsfEM100Specific.counter);
  }

  // 4. EM950 (105) and EM1000 (111).
  constexpr int kEm950Ids[] = {
      GSF_SWATH_BATHY_SUBRECORD_EM950_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_EM1000_SPECIFIC,
  };
  for (int sensor_id : kEm950Ids) {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(/*num_beams=*/2, sensor_id);
    in.sensor_data.gsfEM950Specific.ping_number = 789;
    in.sensor_data.gsfEM950Specific.mode = 2;
    in.sensor_data.gsfEM950Specific.ping_quality = 95;
    in.sensor_data.gsfEM950Specific.ship_pitch = -1.75;
    in.sensor_data.gsfEM950Specific.transducer_pitch = 0.85;
    in.sensor_data.gsfEM950Specific.surface_velocity = 1512.3;
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(sensor_id, out.sensor_id);
    EXPECT_EQ(789, out.sensor_data.gsfEM950Specific.ping_number);
    EXPECT_EQ(95, out.sensor_data.gsfEM950Specific.ping_quality);
    EXPECT_NEAR(-1.75, out.sensor_data.gsfEM950Specific.ship_pitch, 1e-2);
    EXPECT_NEAR(1512.3, out.sensor_data.gsfEM950Specific.surface_velocity,
                1e-1);
  }

  // 5. EM121A (106) and EM121 (107).
  constexpr int kEm121Ids[] = {
      GSF_SWATH_BATHY_SUBRECORD_EM121A_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_EM121_SPECIFIC,
  };
  for (int sensor_id : kEm121Ids) {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(/*num_beams=*/2, sensor_id);
    in.sensor_data.gsfEM121ASpecific.ping_number = 321;
    in.sensor_data.gsfEM121ASpecific.mode = 1;
    in.sensor_data.gsfEM121ASpecific.valid_beams = 120;
    in.sensor_data.gsfEM121ASpecific.pulse_length = 10;
    in.sensor_data.gsfEM121ASpecific.beam_width = 2;
    in.sensor_data.gsfEM121ASpecific.tx_power = 4;
    in.sensor_data.gsfEM121ASpecific.tx_status = 0;
    in.sensor_data.gsfEM121ASpecific.rx_status = 1;
    in.sensor_data.gsfEM121ASpecific.surface_velocity = 1498.5;
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(sensor_id, out.sensor_id);
    EXPECT_EQ(321, out.sensor_data.gsfEM121ASpecific.ping_number);
    EXPECT_EQ(120, out.sensor_data.gsfEM121ASpecific.valid_beams);
    EXPECT_NEAR(1498.5, out.sensor_data.gsfEM121ASpecific.surface_velocity,
                1e-1);
  }

  // 6. SASS (108), TypeIII SeaBeam (112), and CmpSass (121).
  constexpr int kSassIds[] = {
      GSF_SWATH_BATHY_SUBRECORD_SASS_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_TYPEIII_SEABEAM_SPECIFIC,
  };
  for (int sensor_id : kSassIds) {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(/*num_beams=*/2, sensor_id);
    in.sensor_data.gsfSASSSpecific.leftmost_beam = 1;
    in.sensor_data.gsfSASSSpecific.rightmost_beam = 60;
    in.sensor_data.gsfSASSSpecific.total_beams = 60;
    in.sensor_data.gsfSASSSpecific.nav_mode = 2;
    in.sensor_data.gsfSASSSpecific.ping_number = 500;
    in.sensor_data.gsfSASSSpecific.mission_number = 12;
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(sensor_id, out.sensor_id);
    EXPECT_EQ(60, out.sensor_data.gsfSASSSpecific.total_beams);
    EXPECT_EQ(500, out.sensor_data.gsfSASSSpecific.ping_number);
  }
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_CMP_SASS_SPECIFIC);
    in.sensor_data.gsfCmpSassSpecific.lfreq = 4921.2;
    in.sensor_data.gsfCmpSassSpecific.lntens = 123.4;
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_NEAR(4921.2, out.sensor_data.gsfCmpSassSpecific.lfreq, 1e-1);
    EXPECT_NEAR(123.4, out.sensor_data.gsfCmpSassSpecific.lntens, 1e-1);
  }

  // 7. SeaMap (109) for both GSF v3.11 and GSF v2.07 compatibility branch.
  {
    ScopedDecodeTable dec(/*major=*/3, /*minor=*/11);
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_SEAMAP_SPECIFIC);
    in.sensor_data.gsfSeamapSpecific.portTransmitter[0] = 11.1;
    in.sensor_data.gsfSeamapSpecific.portTransmitter[1] = 12.2;
    in.sensor_data.gsfSeamapSpecific.stbdTransmitter[0] = 21.1;
    in.sensor_data.gsfSeamapSpecific.stbdTransmitter[1] = 22.2;
    in.sensor_data.gsfSeamapSpecific.portGain = 15.5;
    in.sensor_data.gsfSeamapSpecific.stbdGain = 16.5;
    in.sensor_data.gsfSeamapSpecific.portPulseLength = 1.2;
    in.sensor_data.gsfSeamapSpecific.stbdPulseLength = 1.3;
    in.sensor_data.gsfSeamapSpecific.pressureDepth = 45.6;
    in.sensor_data.gsfSeamapSpecific.altitude = 78.9;
    in.sensor_data.gsfSeamapSpecific.temperature = 14.2;
    const std::vector<unsigned char> encoded = EncodePing(in, 3, 11);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_NEAR(45.6, out.sensor_data.gsfSeamapSpecific.pressureDepth, 1e-1);
    EXPECT_NEAR(78.9, out.sensor_data.gsfSeamapSpecific.altitude, 1e-1);
    EXPECT_NEAR(14.2, out.sensor_data.gsfSeamapSpecific.temperature, 1e-1);
  }
  {
    ScopedDecodeTable dec(/*major=*/2, /*minor=*/7);
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_SEAMAP_SPECIFIC);
    in.sensor_data.gsfSeamapSpecific.pressureDepth = 45.6;
    in.sensor_data.gsfSeamapSpecific.altitude = 78.9;
    in.sensor_data.gsfSeamapSpecific.temperature = 14.2;
    const std::vector<unsigned char> encoded = EncodePing(in, 2, 7);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                              static_cast<int>(encoded.size())));
    EXPECT_NEAR(78.9, out.sensor_data.gsfSeamapSpecific.pressureDepth, 1e-1);
    EXPECT_NEAR(78.9, out.sensor_data.gsfSeamapSpecific.altitude, 1e-1);
    EXPECT_NEAR(14.2, out.sensor_data.gsfSeamapSpecific.temperature, 1e-1);
  }

  // 8. SeaBat (110), SBAmp (113), SeaBatII (114), SeaBat8101 (115),
  //    SeaBeam2112 (116), ElacMkII (117).
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_SEABAT_SPECIFIC);
    in.sensor_data.gsfSeaBatSpecific.ping_number = 99;
    in.sensor_data.gsfSeaBatSpecific.surface_velocity = 1500.2;
    in.sensor_data.gsfSeaBatSpecific.mode = GSF_SEABAT_WIDE_MODE;
    in.sensor_data.gsfSeaBatSpecific.sonar_range = 100;
    in.sensor_data.gsfSeaBatSpecific.transmit_power = 8;
    in.sensor_data.gsfSeaBatSpecific.receive_gain = 15;
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(99, out.sensor_data.gsfSeaBatSpecific.ping_number);
    EXPECT_NEAR(1500.2, out.sensor_data.gsfSeaBatSpecific.surface_velocity,
                1e-1);
  }
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_SB_AMP_SPECIFIC);
    in.sensor_data.gsfSBAmpSpecific.hour = 14;
    in.sensor_data.gsfSBAmpSpecific.minute = 35;
    in.sensor_data.gsfSBAmpSpecific.second = 22;
    in.sensor_data.gsfSBAmpSpecific.hundredths = 80;
    in.sensor_data.gsfSBAmpSpecific.block_number = 10005;
    in.sensor_data.gsfSBAmpSpecific.avg_gate_depth = 250;
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(14, out.sensor_data.gsfSBAmpSpecific.hour);
    EXPECT_EQ(10005u, out.sensor_data.gsfSBAmpSpecific.block_number);
    EXPECT_EQ(250, out.sensor_data.gsfSBAmpSpecific.avg_gate_depth);
  }
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_SEABAT_II_SPECIFIC);
    in.sensor_data.gsfSeaBatIISpecific.ping_number = 444;
    in.sensor_data.gsfSeaBatIISpecific.surface_velocity = 1521.7;
    in.sensor_data.gsfSeaBatIISpecific.mode = GSF_SEABAT_9002;
    in.sensor_data.gsfSeaBatIISpecific.sonar_range = 200;
    in.sensor_data.gsfSeaBatIISpecific.transmit_power = 7;
    in.sensor_data.gsfSeaBatIISpecific.receive_gain = 18;
    in.sensor_data.gsfSeaBatIISpecific.fore_aft_bw = 1.5;
    in.sensor_data.gsfSeaBatIISpecific.athwart_bw = 1.5;
    in.sensor_data.gsfSeaBatIISpecific.spare[0] = 'S';
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(444, out.sensor_data.gsfSeaBatIISpecific.ping_number);
    EXPECT_NEAR(1.5, out.sensor_data.gsfSeaBatIISpecific.fore_aft_bw, 1e-1);
    EXPECT_EQ('S', out.sensor_data.gsfSeaBatIISpecific.spare[0]);
  }
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_SEABAT_8101_SPECIFIC);
    in.sensor_data.gsfSeaBat8101Specific.ping_number = 555;
    in.sensor_data.gsfSeaBat8101Specific.surface_velocity = 1510.1;
    in.sensor_data.gsfSeaBat8101Specific.mode = GSF_8101_AMPLITUDE;
    in.sensor_data.gsfSeaBat8101Specific.range = 150;
    in.sensor_data.gsfSeaBat8101Specific.power = 6;
    in.sensor_data.gsfSeaBat8101Specific.gain = 22;
    in.sensor_data.gsfSeaBat8101Specific.pulse_width = 75;
    in.sensor_data.gsfSeaBat8101Specific.tvg_spreading = 20;
    in.sensor_data.gsfSeaBat8101Specific.tvg_absorption = 30;
    in.sensor_data.gsfSeaBat8101Specific.fore_aft_bw = 1.5;
    in.sensor_data.gsfSeaBat8101Specific.athwart_bw = 1.5;
    in.sensor_data.gsfSeaBat8101Specific.range_filt_min = 5.0;
    in.sensor_data.gsfSeaBat8101Specific.range_filt_max = 120.0;
    in.sensor_data.gsfSeaBat8101Specific.depth_filt_min = 2.0;
    in.sensor_data.gsfSeaBat8101Specific.depth_filt_max = 100.0;
    in.sensor_data.gsfSeaBat8101Specific.projector = 1;
    in.sensor_data.gsfSeaBat8101Specific.spare[0] = 'P';
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(555, out.sensor_data.gsfSeaBat8101Specific.ping_number);
    EXPECT_EQ(75, out.sensor_data.gsfSeaBat8101Specific.pulse_width);
    EXPECT_EQ('P', out.sensor_data.gsfSeaBat8101Specific.spare[0]);
  }
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_SEABEAM_2112_SPECIFIC);
    in.sensor_data.gsfSeaBeam2112Specific.mode = GSF_2112_SVP_CORRECTION;
    in.sensor_data.gsfSeaBeam2112Specific.surface_velocity = 1500.25;
    in.sensor_data.gsfSeaBeam2112Specific.ssv_source = 'V';
    in.sensor_data.gsfSeaBeam2112Specific.ping_gain = 10;
    in.sensor_data.gsfSeaBeam2112Specific.pulse_width = 7;
    in.sensor_data.gsfSeaBeam2112Specific.transmitter_attenuation = 3;
    in.sensor_data.gsfSeaBeam2112Specific.number_algorithms = 2;
    std::copy_n("WB  ", 5,
                in.sensor_data.gsfSeaBeam2112Specific.algorithm_order);
    in.sensor_data.gsfSeaBeam2112Specific.spare[0] = 'K';
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_NEAR(1500.25,
                out.sensor_data.gsfSeaBeam2112Specific.surface_velocity, 1e-2);
    EXPECT_STREQ("WB  ",
                 out.sensor_data.gsfSeaBeam2112Specific.algorithm_order);
    EXPECT_EQ('K', out.sensor_data.gsfSeaBeam2112Specific.spare[0]);
  }
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_ELAC_MKII_SPECIFIC);
    in.sensor_data.gsfElacMkIISpecific.mode = GSF_MKII_LOW_FREQUENCY;
    in.sensor_data.gsfElacMkIISpecific.ping_num = 666;
    in.sensor_data.gsfElacMkIISpecific.sound_vel = 15000;
    in.sensor_data.gsfElacMkIISpecific.pulse_length = 200;
    in.sensor_data.gsfElacMkIISpecific.receiver_gain_stbd = 12;
    in.sensor_data.gsfElacMkIISpecific.receiver_gain_port = 14;
    in.sensor_data.gsfElacMkIISpecific.reserved = 88;
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(666, out.sensor_data.gsfElacMkIISpecific.ping_num);
    EXPECT_EQ(15000, out.sensor_data.gsfElacMkIISpecific.sound_vel);
    EXPECT_EQ(88, out.sensor_data.gsfElacMkIISpecific.reserved);
  }

  // 9. Swath-embedded single-beam subrecords (206..212).
  constexpr int kSbEchoIds[] = {
      GSF_SWATH_BATHY_SB_SUBRECORD_ECHOTRAC_SPECIFIC,
      GSF_SWATH_BATHY_SB_SUBRECORD_BATHY2000_SPECIFIC,
      GSF_SWATH_BATHY_SB_SUBRECORD_PDD_SPECIFIC,
  };
  for (int sensor_id : kSbEchoIds) {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(/*num_beams=*/1, sensor_id);
    t_gsfSBEchotracSpecific* dst =
        (sensor_id == GSF_SWATH_BATHY_SB_SUBRECORD_PDD_SPECIFIC)
            ? &in.sensor_data.gsfSBPDDSpecific
            : &in.sensor_data.gsfSBEchotracSpecific;
    dst->navigation_error = 15;
    dst->mpp_source = GSF_SB_MPP_SOURCE_GPS_3S;
    dst->tide_source = 3;
    dst->dynamic_draft = -0.35;
    dst->spare[0] = 'E';
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(sensor_id, out.sensor_id);
    const t_gsfSBEchotracSpecific* got =
        (sensor_id == GSF_SWATH_BATHY_SB_SUBRECORD_PDD_SPECIFIC)
            ? &out.sensor_data.gsfSBPDDSpecific
            : &out.sensor_data.gsfSBEchotracSpecific;
    EXPECT_EQ(15, got->navigation_error);
    EXPECT_NEAR(-0.35, got->dynamic_draft, 1e-2);
    EXPECT_EQ('E', got->spare[0]);
  }
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/1, GSF_SWATH_BATHY_SB_SUBRECORD_MGD77_SPECIFIC);
    in.sensor_data.gsfSBMGD77Specific.time_zone_corr = 4;
    in.sensor_data.gsfSBMGD77Specific.position_type_code = 2;
    in.sensor_data.gsfSBMGD77Specific.correction_code = 5;
    in.sensor_data.gsfSBMGD77Specific.bathy_type_code = 3;
    in.sensor_data.gsfSBMGD77Specific.quality_code = 1;
    in.sensor_data.gsfSBMGD77Specific.travel_time = 0.0321;
    in.sensor_data.gsfSBMGD77Specific.spare[0] = 'M';
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_NEAR(0.0321, out.sensor_data.gsfSBMGD77Specific.travel_time, 1e-4);
    EXPECT_EQ('M', out.sensor_data.gsfSBMGD77Specific.spare[0]);
  }
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/1, GSF_SWATH_BATHY_SB_SUBRECORD_BDB_SPECIFIC);
    in.sensor_data.gsfSBBDBSpecific.doc_no = 98765;
    in.sensor_data.gsfSBBDBSpecific.eval = '2';
    in.sensor_data.gsfSBBDBSpecific.classification = 'U';
    in.sensor_data.gsfSBBDBSpecific.track_adj_flag = 'N';
    in.sensor_data.gsfSBBDBSpecific.source_flag = 'O';
    in.sensor_data.gsfSBBDBSpecific.pt_or_track_ln = 'D';
    in.sensor_data.gsfSBBDBSpecific.datum_flag = 'W';
    in.sensor_data.gsfSBBDBSpecific.spare[0] = 'B';
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(98765, out.sensor_data.gsfSBBDBSpecific.doc_no);
    EXPECT_EQ('O', out.sensor_data.gsfSBBDBSpecific.source_flag);
    EXPECT_EQ('B', out.sensor_data.gsfSBBDBSpecific.spare[0]);
  }
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/1, GSF_SWATH_BATHY_SB_SUBRECORD_NOSHDB_SPECIFIC);
    in.sensor_data.gsfSBNOSHDBSpecific.type_code = 7;
    in.sensor_data.gsfSBNOSHDBSpecific.carto_code = 8;
    in.sensor_data.gsfSBNOSHDBSpecific.spare[0] = 'N';
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(7, out.sensor_data.gsfSBNOSHDBSpecific.type_code);
    EXPECT_EQ(8, out.sensor_data.gsfSBNOSHDBSpecific.carto_code);
    EXPECT_EQ('N', out.sensor_data.gsfSBNOSHDBSpecific.spare[0]);
  }
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/1, GSF_SWATH_BATHY_SB_SUBRECORD_NAVISOUND_SPECIFIC);
    in.sensor_data.gsfSBNavisoundSpecific.pulse_length = 4.56;
    in.sensor_data.gsfSBNavisoundSpecific.spare[0] = 'V';
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_NEAR(4.56, out.sensor_data.gsfSBNavisoundSpecific.pulse_length,
                1e-2);
    EXPECT_EQ('V', out.sensor_data.gsfSBNavisoundSpecific.spare[0]);
  }
}

TEST(GsfDecTest, DecodeEM3SpecificAllIdsAndDualRunTimeRaw) {
  // 1. All 9 EM3 sensor IDs via encode/decode round-trip.
  constexpr int kEm3Ids[] = {
      GSF_SWATH_BATHY_SUBRECORD_EM2000_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_EM1002_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_EM300_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_EM120_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_EM3002_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_EM3000D_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_EM3002D_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_EM121A_SIS_SPECIFIC,
  };
  for (int sensor_id : kEm3Ids) {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(/*num_beams=*/2, sensor_id);
    in.sensor_data.gsfEM3Specific.model_number = 3002;
    in.sensor_data.gsfEM3Specific.ping_number = 1001;
    in.sensor_data.gsfEM3Specific.serial_number = 205;
    in.sensor_data.gsfEM3Specific.surface_velocity = 1505.5;
    in.sensor_data.gsfEM3Specific.transducer_depth = 4.25;
    in.sensor_data.gsfEM3Specific.valid_beams = 254;
    in.sensor_data.gsfEM3Specific.sample_rate = 14286;
    in.sensor_data.gsfEM3Specific.depth_difference = 0.15;
    in.sensor_data.gsfEM3Specific.offset_multiplier = 1;
    auto& rt0 = in.sensor_data.gsfEM3Specific.run_time[0];
    rt0.model_number = 3002;
    rt0.dg_time.tv_sec = 1700001000;
    rt0.dg_time.tv_nsec = 500000000;
    rt0.ping_number = 1001;
    rt0.serial_number = 205;
    rt0.system_status = 7;
    rt0.mode = 1;
    rt0.filter_id = 2;
    rt0.min_depth = 5.0;
    rt0.max_depth = 200.0;
    rt0.absorption = 15.25;
    rt0.pulse_length = 100.0;
    rt0.transmit_beam_width = 1.5;
    rt0.power_reduction = 3;
    rt0.receive_beam_width = 1.5;
    rt0.receive_bandwidth = 2000;
    rt0.receive_gain = 10;
    rt0.cross_over_angle = 40;
    rt0.ssv_source = 1;
    rt0.port_swath_width = 120;
    rt0.beam_spacing = 1;
    rt0.port_coverage_sector = 60;
    rt0.stabilization = 3;
    rt0.stbd_coverage_sector = 65;
    rt0.stbd_swath_width = 130;
    rt0.hilo_freq_absorp_ratio = 20;

    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(sensor_id, out.sensor_id);
    EXPECT_EQ(3002, out.sensor_data.gsfEM3Specific.model_number);
    EXPECT_EQ(250, out.sensor_data.gsfEM3Specific.run_time[0].swath_width);
    EXPECT_EQ(125, out.sensor_data.gsfEM3Specific.run_time[0].coverage_sector);
  }

  // 2. Raw EM3Specific with run_time_id == 0 (no run-time parameter blocks).
  {
    ScopedDecodeTable dec;
    std::vector<unsigned char> raw;
    AppendPingHeaderV3(raw, /*num_beams=*/2);
    AppendSubrecordHeader(raw, GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC, 21u);
    AppendU16Be(raw, 3000u);   // model_number
    AppendU16Be(raw, 111u);    // ping_number
    AppendU16Be(raw, 222u);    // serial_number
    AppendU16Be(raw, 15000u);  // surface_velocity
    AppendU16Be(raw, 350u);    // transducer_depth
    AppendU16Be(raw, 127u);    // valid_beams
    AppendU16Be(raw, 14000u);  // sample_rate
    AppendI16Be(raw, -25);     // depth_difference
    AppendU8(raw, 2);          // offset_multiplier
    AppendU32Be(raw, 0u);      // run_time_id == 0

    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    out.sensor_data.gsfEM3Specific.run_time[0].model_number = -1;
    ASSERT_EQ(static_cast<int>(raw.size()),
              DecodeSwathPingPadded(&out, raw.data(), &dec.ft, kHandle,
                                    static_cast<int>(raw.size())));
    EXPECT_EQ(3000, out.sensor_data.gsfEM3Specific.model_number);
    EXPECT_EQ(-1, out.sensor_data.gsfEM3Specific.run_time[0].model_number);
  }

  // 3. Raw EM3000D with run_time_id == 3 (both run_time[0] and run_time[1])
  //    covering zero and nonzero stbd_swath_width/stbd_coverage_sector paths.
  auto append_rt = [](std::vector<unsigned char>& out, std::uint16_t model,
                      std::uint16_t port_w, std::uint16_t stbd_w,
                      std::uint8_t port_cov, std::uint8_t stbd_cov,
                      bool omit_trailing_increment) {
    AppendU16Be(out, model);
    AppendU32Be(out, 1700001234u);
    AppendU32Be(out, 567000000u);
    AppendU16Be(out, 42u);
    AppendU16Be(out, 333u);
    AppendU32Be(out, 0u);
    AppendU8(out, 1);
    AppendU8(out, 2);
    AppendU16Be(out, 10u);
    AppendU16Be(out, 150u);
    AppendU16Be(out, 1200u);
    AppendU16Be(out, 80u);
    AppendU16Be(out, 15u);
    AppendU8(out, 3);
    AppendU8(out, 15);
    AppendU8(out, 40);
    AppendU8(out, 8);
    AppendU8(out, 35);
    AppendU8(out, 1);
    AppendU16Be(out, port_w);
    AppendU8(out, 1);
    AppendU8(out, port_cov);
    AppendU8(out, 2);
    AppendU8(out, stbd_cov);
    AppendU16Be(out, stbd_w);
    AppendU8(out, 19);
    // DecodeEM3Specific omits p += 1 after hilo_freq_absorp_ratio in
    // run_time[1].
    out.insert(out.end(), omit_trailing_increment ? 3 : 4, 0);
  };

  for (bool zero_in_rt0 : {true, false}) {
    ScopedDecodeTable dec;
    std::vector<unsigned char> raw;
    AppendPingHeaderV3(raw, /*num_beams=*/2);
    AppendSubrecordHeader(raw, GSF_SWATH_BATHY_SUBRECORD_EM3000D_SPECIFIC,
                          118u);
    AppendU16Be(raw, 3000u);
    AppendU16Be(raw, 55u);
    AppendU16Be(raw, 777u);
    AppendU16Be(raw, 15010u);
    AppendU16Be(raw, 250u);
    AppendU16Be(raw, 254u);
    AppendU16Be(raw, 14286u);
    AppendI16Be(raw, 15);
    AppendU8(raw, 1);
    AppendU32Be(raw, 3u);  // run_time_id == 0x3
    if (zero_in_rt0) {
      append_rt(raw, 3000u, 200u, 0u, 120u, 0u, false);
      append_rt(raw, 3001u, 110u, 130u, 55u, 65u, true);
    } else {
      append_rt(raw, 3000u, 100u, 120u, 50u, 60u, false);
      append_rt(raw, 3001u, 240u, 0u, 130u, 0u, true);
    }

    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(static_cast<int>(raw.size()),
              DecodeSwathPingPadded(&out, raw.data(), &dec.ft, kHandle,
                                           static_cast<int>(raw.size())));
    EXPECT_EQ(3000, out.sensor_data.gsfEM3Specific.run_time[0].model_number);
    EXPECT_EQ(3001, out.sensor_data.gsfEM3Specific.run_time[1].model_number);
    if (zero_in_rt0) {
      EXPECT_EQ(100,
                out.sensor_data.gsfEM3Specific.run_time[0].port_swath_width);
      EXPECT_EQ(
          60, out.sensor_data.gsfEM3Specific.run_time[0].stbd_coverage_sector);
      EXPECT_EQ(240, out.sensor_data.gsfEM3Specific.run_time[1].swath_width);
    } else {
      EXPECT_EQ(220, out.sensor_data.gsfEM3Specific.run_time[0].swath_width);
      EXPECT_EQ(120,
                out.sensor_data.gsfEM3Specific.run_time[1].port_swath_width);
      EXPECT_EQ(
          65, out.sensor_data.gsfEM3Specific.run_time[1].stbd_coverage_sector);
    }
  }
}

TEST(GsfDecTest, DecodeEM3RawSpecificAllIdsModelsAndSectorClamp) {
  // 1. All 9 EM3 Raw IDs + model_number switch branches (1002, 3000, 2000).
  struct Em3RawCase {
    int sensor_id;
    int rt_model;
  };
  constexpr Em3RawCase kCases[] = {
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM2000_RAW_SPECIFIC,
       .rt_model = 2000},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM3000_RAW_SPECIFIC,
       .rt_model = 3000},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM1002_RAW_SPECIFIC,
       .rt_model = 1002},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM300_RAW_SPECIFIC,
       .rt_model = 300},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM120_RAW_SPECIFIC,
       .rt_model = 120},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM3002_RAW_SPECIFIC,
       .rt_model = 3020},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM3000D_RAW_SPECIFIC,
       .rt_model = 3000},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM3002D_RAW_SPECIFIC,
       .rt_model = 3020},
      {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM121A_SIS_RAW_SPECIFIC,
       .rt_model = 121},
  };

  for (const auto& tc : kCases) {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(/*num_beams=*/2, tc.sensor_id);
    auto& raw = in.sensor_data.gsfEM3RawSpecific;
    raw.model_number = tc.rt_model;
    raw.ping_counter = 2002;
    raw.serial_number = 303;
    raw.surface_velocity = 1508.2;
    raw.transducer_depth = 3.125;
    raw.valid_detections = 128;
    raw.sampling_frequency = 25000.5;
    raw.vehicle_depth = 12.345;
    raw.depth_difference = 0.25;
    raw.offset_multiplier = 1;
    raw.transmit_sectors = 2;
    for (int i = 0; i < 2; ++i) {
      raw.sector[i].tilt_angle = -1.5 + i * 3.0;
      raw.sector[i].focus_range = 50.0 + i * 25.0;
      raw.sector[i].signal_length = 0.0005 + i * 0.0002;
      raw.sector[i].transmit_delay = 0.001 + i * 0.0005;
      raw.sector[i].center_frequency = 300000.0 + i * 5000.0;
      raw.sector[i].waveform_id = GSF_EM_WAVEFORM_FM_UP;
      raw.sector[i].sector_number = i;
      raw.sector[i].signal_bandwidth = 4000.0;
    }
    PopulateEmRunTime(raw.run_time, tc.rt_model);
    PopulateEmPuStatus(raw.pu_status);

    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(tc.sensor_id, out.sensor_id);
    EXPECT_EQ(2, out.sensor_data.gsfEM3RawSpecific.transmit_sectors);
    EXPECT_NEAR(25000.5, out.sensor_data.gsfEM3RawSpecific.sampling_frequency,
                1e-6);
    if (tc.rt_model == 1002) {
      EXPECT_NEAR(1495.5,
                  out.sensor_data.gsfEM3RawSpecific.run_time.durotong_speed,
                  1e-1);
    } else if (tc.rt_model == 3000 || tc.rt_model == 300 ||
               tc.rt_model == 120 || tc.rt_model == 3020) {
      EXPECT_NEAR(-2.25,
                  out.sensor_data.gsfEM3RawSpecific.run_time.tx_along_tilt,
                  1e-2);
    }
  }

  // 2. Clamp transmit_sectors > GSF_MAX_EM3_SECTORS (20).
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_EM3000_RAW_SPECIFIC);
    in.sensor_data.gsfEM3RawSpecific.transmit_sectors = GSF_MAX_EM3_SECTORS;
    PopulateEmRunTime(in.sensor_data.gsfEM3RawSpecific.run_time, 3000);
    PopulateEmPuStatus(in.sensor_data.gsfEM3RawSpecific.pu_status);

    std::vector<unsigned char> encoded = EncodePing(in);
    // Offset: 56-byte v3 ping header + 4-byte subrec header + 45 bytes = 105.
    ASSERT_GT(encoded.size(), 106u);
    ASSERT_EQ(GSF_MAX_EM3_SECTORS, (encoded[105] << 8) | encoded[106]);
    encoded[106] = GSF_MAX_EM3_SECTORS + 1;

    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(GSF_MAX_EM3_SECTORS,
              out.sensor_data.gsfEM3RawSpecific.transmit_sectors);
  }
}

TEST(GsfDecTest, DecodeEM4SpecificAllIdsAndSectorClamp) {
  constexpr int kEm4Ids[] = {
      GSF_SWATH_BATHY_SUBRECORD_EM710_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_EM302_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_EM122_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_EM2040_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_ME70BO_SPECIFIC,
  };

  for (int sensor_id : kEm4Ids) {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(/*num_beams=*/2, sensor_id);
    auto& em4 = in.sensor_data.gsfEM4Specific;
    em4.model_number = 710;
    em4.ping_counter = 3003;
    em4.serial_number = 404;
    em4.surface_velocity = 1515.4;
    em4.transducer_depth = 5.5;
    em4.valid_detections = 400;
    em4.sampling_frequency = 50000.125;
    em4.doppler_corr_scale = 100;
    em4.vehicle_depth = 20.5;
    em4.transmit_sectors = 2;
    for (int i = 0; i < 2; ++i) {
      em4.sector[i].tilt_angle = -2.0 + i * 4.0;
      em4.sector[i].focus_range = 75.0;
      em4.sector[i].signal_length = 0.001;
      em4.sector[i].transmit_delay = 0.002;
      em4.sector[i].center_frequency = 85000.0;
      em4.sector[i].mean_absorption = 24.5;
      em4.sector[i].waveform_id = GSF_EM_WAVEFORM_CW;
      em4.sector[i].sector_number = i;
      em4.sector[i].signal_bandwidth = 3500.0;
    }
    PopulateEmRunTime(em4.run_time, 710);
    PopulateEmPuStatus(em4.pu_status);

    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(sensor_id, out.sensor_id);
    EXPECT_EQ(2, out.sensor_data.gsfEM4Specific.transmit_sectors);
    EXPECT_NEAR(24.5, out.sensor_data.gsfEM4Specific.sector[0].mean_absorption,
                1e-2);
    EXPECT_NEAR(-2.25, out.sensor_data.gsfEM4Specific.run_time.tx_along_tilt,
                1e-2);
    EXPECT_EQ(2, out.sensor_data.gsfEM4Specific.run_time.filter_id_2);
  }

  // Clamp transmit_sectors > GSF_MAX_EM4_SECTORS (9).
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_EM710_SPECIFIC);
    in.sensor_data.gsfEM4Specific.transmit_sectors = GSF_MAX_EM4_SECTORS;
    PopulateEmRunTime(in.sensor_data.gsfEM4Specific.run_time, 710);
    PopulateEmPuStatus(in.sensor_data.gsfEM4Specific.pu_status);

    std::vector<unsigned char> encoded = EncodePing(in);
    // Offset: 56 + 4 + 46 = 106.
    ASSERT_GT(encoded.size(), 107u);
    ASSERT_EQ(GSF_MAX_EM4_SECTORS, (encoded[106] << 8) | encoded[107]);
    encoded[107] = GSF_MAX_EM4_SECTORS + 1;

    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(GSF_MAX_EM4_SECTORS,
              out.sensor_data.gsfEM4Specific.transmit_sectors);
  }
}

TEST(GsfDecTest, DecodeKMALLSpecificRoundTripAndClamps) {
  auto make_kmall_ping = [](int num_sectors, int num_classes) {
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_KMALL_SPECIFIC);
    auto& km = in.sensor_data.gsfKMALLSpecific;
    km.dgmType = GSF_KMALL_MRZ;
    km.dgmVersion = 1;
    km.systemID = 2;
    km.echoSounderID = 2040;
    km.numBytesCmnPart = 28;
    km.pingCnt = 505;
    km.rxFansPerPing = 1;
    km.rxFanIndex = 0;
    km.swathsPerPing = 1;
    km.swathAlongPosition = 0;
    km.txTransducerInd = 0;
    km.rxTransducerInd = 0;
    km.numRxTransducers = 1;
    km.algorithmType = 0;
    km.numBytesInfoData = 140;
    km.pingRate_Hz = 4.25;
    km.beamSpacing = 2;
    km.depthMode = 1;
    km.subDepthMode = 0;
    km.distanceBtwSwath = 100;
    km.detectionMode = 0;
    km.pulseForm = 2;
    km.frequencyMode_Hz = 300000.0;
    km.freqRangeLowLim_Hz = 280000.0;
    km.freqRangeHighLim_Hz = 320000.0;
    km.maxTotalTxPulseLength_sec = 0.002;
    km.maxEffTxPulseLength_sec = 0.0015;
    km.maxEffTxBandWidth_Hz = 10000.0;
    km.absCoeff_dBPerkm = 65.5;
    km.portSectorEdge_deg = -65.0;
    km.starbSectorEdge_deg = 65.0;
    km.portMeanCov_deg = -60.5;
    km.starbMeanCov_deg = 60.5;
    km.portMeanCov_m = -150.0;
    km.starbMeanCov_m = 150.0;
    km.modeAndStabilisation = 3;
    km.runtimeFilter1 = 1;
    km.runtimeFilter2 = 2;
    km.pipeTrackingStatus = 0;
    km.transmitArraySizeUsed_deg = 0.75;
    km.receiveArraySizeUsed_deg = 0.75;
    km.transmitPower_dB = 0.0;
    km.SLrampUpTimeRemaining = 0;
    km.yawAngle_deg = -0.5;
    km.numTxSectors = num_sectors;
    km.numBytesPerTxSector = 48;
    km.headingVessel_deg = 123.456;
    km.soundSpeedAtTxDepth_mPerSec = 1504.25;
    km.txTransducerDepth_m = 4.5;
    km.z_waterLevelReRefPoint_m = -1.25;
    km.x_kmallToall_m = 0.5;
    km.y_kmallToall_m = -0.25;
    km.latLongInfo = 1;
    km.posSensorStatus = 0;
    km.attitudeSensorStatus = 0;
    km.latitude_deg = 36.8;
    km.longitude_deg = -121.9;
    km.ellipsoidHeightReRefPoint_m = 10.5;
    for (int i = 0; i < num_sectors; ++i) {
      km.sector[i].txSectorNumb = i;
      km.sector[i].txArrNumber = 0;
      km.sector[i].txSubArray = i % 3;
      km.sector[i].sectorTransmitDelay_sec = 0.001 * i;
      km.sector[i].tiltAngleReTx_deg = -1.0 + i * 0.5;
      km.sector[i].txNominalSourceLevel_dB = 210.5;
      km.sector[i].txFocusRange_m = 50.0;
      km.sector[i].centreFreq_Hz = 300000.0;
      km.sector[i].signalBandWidth_Hz = 5000.0;
      km.sector[i].totalSignalLength_sec = 0.002;
      km.sector[i].pulseShading = 50;
      km.sector[i].signalWaveForm = 2;
      km.sector[i].highVoltageLevel_dB = -3.0f;
      km.sector[i].sectorTrackiongCorr_dB = 0.5f;
      km.sector[i].effectiveSignalLength_sec = 0.0015f;
    }
    km.numBytesRxInfo = 32;
    km.numSoundingsMaxMain = 512;
    km.numSoundingsValidMain = 500;
    km.numBytesPerSounding = 120;
    km.WCSampleRate = 25000.25;
    km.seabedImageSampleRate = 12500.5;
    km.BSnormal_dB = -18.5;
    km.BSoblique_dB = -24.5;
    km.extraDetectionAlarmFlag = 1;
    km.numExtraDetections = 3;
    km.numExtraDetectionClasses = num_classes;
    km.numBytesPerClass = 4;
    for (int i = 0; i < num_classes; ++i) {
      km.extraDetClassInfo[i].numExtraDetInClass = i + 1;
      km.extraDetClassInfo[i].alarmFlag = i % 2;
    }
    return in;
  };

  // 1. Round-trip with 2 sectors and 2 extra detection classes.
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in =
        make_kmall_ping(/*num_sectors=*/2, /*num_classes=*/2);
    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    const auto& km = out.sensor_data.gsfKMALLSpecific;
    EXPECT_EQ(2040, km.echoSounderID);
    EXPECT_EQ(2, km.numTxSectors);
    EXPECT_EQ(2, km.numExtraDetectionClasses);
    EXPECT_NEAR(25000.25, km.WCSampleRate, 1e-6);
    EXPECT_NEAR(-18.5, km.BSnormal_dB, 1e-6);
    EXPECT_NEAR(-3.0, km.sector[0].highVoltageLevel_dB, 1e-4);
    EXPECT_EQ(2, km.extraDetClassInfo[1].numExtraDetInClass);
  }

  // 2. Clamp numTxSectors > GSF_MAX_KMALL_SECTORS (9) and
  //    numExtraDetectionClasses > GSF_MAX_KMALL_EXTRA_CLASSES (11).
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in =
        make_kmall_ping(GSF_MAX_KMALL_SECTORS, GSF_MAX_KMALL_EXTRA_CLASSES);
    std::vector<unsigned char> encoded = EncodePing(in);
    ASSERT_GT(encoded.size(), 770u);
    ASSERT_EQ(GSF_MAX_KMALL_SECTORS, (encoded[178] << 8) | encoded[179]);
    ASSERT_EQ(GSF_MAX_KMALL_EXTRA_CLASSES, (encoded[766] << 8) | encoded[767]);
    encoded[179] = GSF_MAX_KMALL_SECTORS + 1;
    encoded[767] = GSF_MAX_KMALL_EXTRA_CLASSES + 1;

    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(GSF_MAX_KMALL_SECTORS,
              out.sensor_data.gsfKMALLSpecific.numTxSectors);
    EXPECT_EQ(GSF_MAX_KMALL_EXTRA_CLASSES,
              out.sensor_data.gsfKMALLSpecific.numExtraDetectionClasses);
  }
}

TEST(GsfDecTest, DecodeReson8100And7100AndTSeriesSpecific) {
  // 1. All 6 Reson 81xx sensor IDs.
  constexpr int kReson8100Ids[] = {
      GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_RESON_8111_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_RESON_8124_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_RESON_8125_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_RESON_8150_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_RESON_8160_SPECIFIC,
  };
  for (int sensor_id : kReson8100Ids) {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(/*num_beams=*/2, sensor_id);
    auto& r8 = in.sensor_data.gsfReson8100Specific;
    r8.latency = 12;
    r8.ping_number = 54321;
    r8.sonar_id = 0x01020304;
    r8.sonar_model = 8125;
    r8.frequency = 455;
    r8.surface_velocity = 1503.4;
    r8.sample_rate = 10000;
    r8.ping_rate = 15000;
    r8.mode = GSF_8100_PITCH_STAB | GSF_8100_ROLL_STAB;
    r8.range = 100;
    r8.power = 5;
    r8.gain = 20;
    r8.pulse_width = 50;
    r8.tvg_spreading = 24;
    r8.tvg_absorption = 36;
    r8.fore_aft_bw = 1.0;
    r8.athwart_bw = 0.5;
    r8.projector_type = 2;
    r8.projector_angle = -150;
    r8.range_filt_min = 5;
    r8.range_filt_max = 95;
    r8.depth_filt_min = 3;
    r8.depth_filt_max = 90;
    r8.filters_active = 3;
    r8.temperature = 145;
    r8.beam_spacing = 0.5;
    r8.spare[0] = 'R';

    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(sensor_id, out.sensor_id);
    EXPECT_EQ(-150, out.sensor_data.gsfReson8100Specific.projector_angle);
    EXPECT_NEAR(0.5, out.sensor_data.gsfReson8100Specific.beam_spacing, 1e-4);
    EXPECT_EQ('R', out.sensor_data.gsfReson8100Specific.spare[0]);
  }

  // 2. Reson 7100 (138).
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC);
    auto& r7 = in.sensor_data.gsfReson7100Specific;
    r7.protocol_version = 5;
    r7.device_id = 7125;
    r7.major_serial_number = 100;
    r7.minor_serial_number = 200;
    r7.ping_number = 9876;
    r7.multi_ping_seq = 1;
    r7.frequency = 400000.0;
    r7.sample_rate = 34482.75;
    r7.receiver_bandwdth = 10000.5;
    r7.tx_pulse_width = 0.0001;
    r7.tx_pulse_type_id = 1;
    r7.tx_pulse_envlp_id = 1;
    r7.tx_pulse_envlp_param = 0.25;
    r7.max_ping_rate = 20.5;
    r7.ping_period = 0.05;
    r7.range = 150.25;
    r7.power = 210.5;
    r7.gain = 6.5;
    r7.control_flags = 0x10;
    r7.projector_id = 2;
    r7.projector_steer_angl_vert = 1.25;
    r7.projector_steer_angl_horz = 0.75;
    r7.projector_beam_wdth_vert = 1.0;
    r7.projector_beam_wdth_horz = 2.0;
    r7.projector_beam_focal_pt = 50.0;
    r7.projector_beam_weighting_window_type = 1;
    r7.projector_beam_weighting_window_param = 3;
    r7.transmit_flags = GSF_7100_PITCH_STAB;
    r7.hydrophone_id = 4;
    r7.receiving_beam_weighting_window_type = 1;
    r7.receiving_beam_weighting_window_param = 5;
    r7.receive_flags = GSF_7100_ROLL_STAB;
    r7.receive_beam_width = 0.5;
    r7.range_filt_min = 5.0;
    r7.range_filt_max = 140.0;
    r7.depth_filt_min = 2.0;
    r7.depth_filt_max = 130.0;
    r7.absorption = 45.25;
    r7.sound_velocity = 1502.5;
    r7.spreading = 20.125;
    r7.raw_data_from_7027 = 1;
    r7.sv_source = 0;
    r7.layer_comp_flag = 1;

    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(7125u, out.sensor_data.gsfReson7100Specific.device_id);
    EXPECT_NEAR(6.5, out.sensor_data.gsfReson7100Specific.gain, 1e-2);
    EXPECT_NEAR(1.25,
                out.sensor_data.gsfReson7100Specific.projector_steer_angl_vert,
                1e-3);
    EXPECT_EQ(1, out.sensor_data.gsfReson7100Specific.raw_data_from_7027);
    EXPECT_EQ(1, out.sensor_data.gsfReson7100Specific.layer_comp_flag);
  }

  // 3. Reson TSeries (155), testing sound_velocity > 0 override branch and ==
  // 0.
  for (double sv : {1504.123456, 0.0}) {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_RESON_TSERIES_SPECIFIC);
    auto& rt = in.sensor_data.gsfResonTSeriesSpecific;
    rt.protocol_version = 6;
    rt.device_id = 50;
    rt.number_devices = 2;
    rt.system_enumerator = 1;
    rt.major_serial_number = 11;
    rt.minor_serial_number = 22;
    rt.ping_number = 333;
    rt.multi_ping_seq = 0;
    rt.frequency = 400000.0;
    rt.sample_rate = 34000.0;
    rt.receiver_bandwdth = 12000.0;
    rt.tx_pulse_width = 0.0002;
    rt.tx_pulse_type_id = 1;
    rt.tx_pulse_envlp_id = 1;
    rt.tx_pulse_envlp_param = 0.5;
    rt.tx_pulse_mode = 2;
    rt.tx_pulse_reserved = 3;
    rt.max_ping_rate = 15.0;
    rt.ping_period = 0.066;
    rt.range = 100.0;
    rt.power = 215.0;
    rt.gain = 3.5;
    rt.control_flags = 0x20;
    rt.projector_id = 1;
    rt.projector_steer_angl_vert = 0.5;
    rt.projector_steer_angl_horz = 0.5;
    rt.projector_beam_wdth_vert = 1.0;
    rt.projector_beam_wdth_horz = 1.5;
    rt.projector_beam_focal_pt = 40.0;
    rt.projector_beam_weighting_window_type = 1;
    rt.projector_beam_weighting_window_param = 2.0;
    rt.transmit_flags = 1;
    rt.hydrophone_id = 2;
    rt.receiving_beam_weighting_window_type = 1;
    rt.receiving_beam_weighting_window_param = 3.0;
    rt.receive_flags = 5;
    rt.receive_beam_width = 0.5;
    rt.range_filt_min = 5.0;
    rt.range_filt_max = 95.0;
    rt.depth_filt_min = -1.5;
    rt.depth_filt_max = 90.5;
    rt.absorption = 30.0;
    rt.sound_velocity = sv;
    rt.sv_source = 1;
    rt.spreading = 20.0;
    rt.beam_spacing_mode = 2;
    rt.sonar_source_mode = 0;
    rt.coverage_mode = 1;
    rt.coverage_angle = 140.0;
    rt.horizontal_receiver_steering_angle = -2.5;
    rt.uncertainty_type = 1;
    rt.transmitter_steering_angle = -0.015;
    rt.applied_roll = 0.025;
    rt.detection_algorithm = 4;
    rt.detection_flags = 0xAA;
    std::copy_n("T50-R", 6, rt.device_description);
    rt.match_filter_control = 1;
    rt.match_filter_start_freq = 380000.0;
    rt.match_filter_end_freq = 420000.0;
    rt.match_filter_window_type = 2;
    rt.match_filter_shading_value = 1.25;
    rt.match_filter_effect_pulse_width = 0.00005;

    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    const auto& got = out.sensor_data.gsfResonTSeriesSpecific;
    EXPECT_EQ(50u, got.device_id);
    EXPECT_NEAR(sv, got.sound_velocity, 1e-6);
    EXPECT_NEAR(-1.5, got.depth_filt_min, 1e-1);
    EXPECT_STREQ("T50-R", got.device_description);
    EXPECT_NEAR(1.25, got.match_filter_shading_value, 1e-4);
  }
}

TEST(GsfDecTest, DecodeGeoSwathKleinDeltaTAndR2SonicSpecific) {
  // 1. GeoSwathPlus (136).
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_GEOSWATH_PLUS_SPECIFIC);
    auto& gs = in.sensor_data.gsfGeoSwathPlusSpecific;
    gs.data_source = 1;
    gs.side = GSF_GEOSWATH_PLUS_STBD_PING;
    gs.model_number = 250;
    gs.frequency = 250000.0;
    gs.echosounder_type = 2;
    gs.ping_number = 70000;
    gs.num_nav_samples = 10;
    gs.num_attitude_samples = 20;
    gs.num_heading_samples = 15;
    gs.num_miniSVS_samples = 5;
    gs.num_echosounder_samples = 2;
    gs.num_raa_samples = 1024;
    gs.mean_sv = 1502.5;
    gs.surface_velocity = 1503.0;
    gs.valid_beams = 256;
    gs.sample_rate = 100000.0;
    gs.pulse_length = 64.0;
    gs.ping_length = 80;
    gs.transmit_power = 4;
    gs.sidescan_gain_channel = 2;
    gs.stabilization = 1;
    gs.gps_quality = 4;
    gs.range_uncertainty = 0.025;
    gs.angle_uncertainty = 0.15;
    gs.spare[0] = 'G';

    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(70000, out.sensor_data.gsfGeoSwathPlusSpecific.ping_number);
    EXPECT_NEAR(250000.0, out.sensor_data.gsfGeoSwathPlusSpecific.frequency,
                10.0);
    EXPECT_NEAR(
        0.025, out.sensor_data.gsfGeoSwathPlusSpecific.range_uncertainty, 1e-3);
    EXPECT_EQ('G', out.sensor_data.gsfGeoSwathPlusSpecific.spare[0]);
  }

  // 2. Klein 5410 BSS (137).
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_KLEIN_5410_BSS_SPECIFIC);
    auto& kl = in.sensor_data.gsfKlein5410BssSpecific;
    kl.data_source = 0;
    kl.side = PORT_PING;
    kl.model_number = 5410;
    kl.acoustic_frequency = 455000.0;
    kl.sampling_frequency = 23437.5;
    kl.ping_number = 123456;
    kl.num_samples = 2048;
    kl.num_raa_samples = 512;
    kl.error_flags = 0x04;
    kl.range = 100;
    kl.fish_depth = 15.25;
    kl.fish_altitude = 12.75;
    kl.sound_speed = 1498.5;
    kl.tx_waveform = 1;
    kl.altimeter = 1;
    kl.raw_data_config = 7;
    kl.spare[0] = 'K';

    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(5410, out.sensor_data.gsfKlein5410BssSpecific.model_number);
    EXPECT_NEAR(15.25, out.sensor_data.gsfKlein5410BssSpecific.fish_depth,
                1e-3);
    EXPECT_EQ('K', out.sensor_data.gsfKlein5410BssSpecific.spare[0]);
  }

  // 3. Imagenex Delta T (150).
  {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(
        /*num_beams=*/2, GSF_SWATH_BATHY_SUBRECORD_DELTA_T_SPECIFIC);
    auto& dt = in.sensor_data.gsfDeltaTSpecific;
    std::copy_n(".83P", 4, dt.decode_file_type);
    dt.version = 3;
    dt.ping_byte_size = 736;
    dt.interrogation_time.tv_sec = 1700010000;
    dt.interrogation_time.tv_nsec = 222000000;
    dt.samples_per_beam = 500;
    dt.sector_size = 120.0;
    dt.start_angle = -60.5;
    dt.angle_increment = 0.5;
    dt.acoustic_range = 50;
    dt.acoustic_frequency = 260;
    dt.sound_velocity = 1500.5;
    dt.range_resolution = 10.0;
    dt.profile_tilt_angle = 15.0;
    dt.repetition_rate = 100.0;
    dt.ping_number = 9001;
    dt.intensity_flag = 1;
    dt.ping_latency = 0.0125;
    dt.data_latency = 0.0250;
    dt.sample_rate_flag = 1;
    dt.option_flags = 3;
    dt.num_pings_avg = 4;
    dt.center_ping_time_offset = 0.05;
    dt.user_defined_byte = 9;
    dt.altitude = 18.25;
    dt.external_sensor_flags = 15;
    dt.pulse_length = 0.00012;
    dt.fore_aft_beamwidth = 1.5;
    dt.athwartships_beamwidth = 1.2;

    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    const auto& got = out.sensor_data.gsfDeltaTSpecific;
    EXPECT_EQ(3, got.version);
    EXPECT_NEAR(-60.5, got.start_angle, 1e-2);
    EXPECT_NEAR(15.0, got.profile_tilt_angle, 1e-3);
    EXPECT_EQ(9001u, got.ping_number);
    EXPECT_NEAR(18.25, got.altitude, 1e-2);
    EXPECT_NEAR(1.5, got.fore_aft_beamwidth, 1e-1);
  }

  // 4. All 3 R2Sonic sensor IDs (2020, 2022, 2024).
  constexpr int kR2SonicIds[] = {
      GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2020_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2022_SPECIFIC,
      GSF_SWATH_BATHY_SUBRECORD_R2SONIC_2024_SPECIFIC,
  };
  for (int sensor_id : kR2SonicIds) {
    ScopedDecodeTable dec;
    gsfSwathBathyPing in = MakeBasePing(/*num_beams=*/2, sensor_id);
    auto& r2 = in.sensor_data.gsfR2SonicSpecific;
    std::copy_n("2024", 5, r2.model_number);
    std::copy_n("554433", 7, r2.serial_number);
    r2.dg_time.tv_sec = 1700011000;
    r2.dg_time.tv_nsec = 400000000;
    r2.ping_number = 7777;
    r2.ping_period = 0.125;
    r2.sound_speed = 1501.25;
    r2.frequency = 400000.0;
    r2.tx_power = 212.5;
    r2.tx_pulse_width = 0.00015;
    r2.tx_beamwidth_vert = 1.0;
    r2.tx_beamwidth_horiz = 2.0;
    r2.tx_steering_vert = 0.25;
    r2.tx_steering_horiz = 0.5;
    r2.tx_misc_info = 1;
    r2.rx_bandwidth = 60000.0;
    r2.rx_sample_rate = 62500.0;
    r2.rx_range = 100.5;
    r2.rx_gain = 14.0;
    r2.rx_spreading = 20.0;
    r2.rx_absorption = 30.5;
    r2.rx_mount_tilt = 1.5;
    r2.rx_misc_info = 2;
    r2.reserved = 3;
    r2.num_beams = 256;
    for (int i = 0; i < 6; ++i) {
      r2.A0_more_info[i] = 1.0 + i * 0.5;
      r2.A2_more_info[i] = 2.0 - i * 0.25;
    }
    r2.G0_depth_gate_min = 0.01;
    r2.G0_depth_gate_max = 0.25;
    r2.G0_depth_gate_slope = 1.25;

    const std::vector<unsigned char> encoded = EncodePing(in);
    gsfSwathBathyPing out;
    std::memset(&out, 0, sizeof(out));
    ASSERT_EQ(
        static_cast<int>(encoded.size()),
        DecodeSwathPingPadded(&out, encoded.data(), &dec.ft, kHandle,
                                     static_cast<int>(encoded.size())));
    EXPECT_EQ(sensor_id, out.sensor_id);
    EXPECT_EQ(7777u, out.sensor_data.gsfR2SonicSpecific.ping_number);
    EXPECT_NEAR(0.25, out.sensor_data.gsfR2SonicSpecific.tx_steering_vert,
                1e-6);
    EXPECT_NEAR(1.5, out.sensor_data.gsfR2SonicSpecific.rx_mount_tilt, 1e-6);
    EXPECT_NEAR(1.25, out.sensor_data.gsfR2SonicSpecific.G0_depth_gate_slope,
                1e-6);
  }
}

// TODO(schwehr): Remaining ~2.7% uncovered lines in src/gsf_dec.c are
// calloc/realloc OOM branches and defensive default cases unreachable from
// valid subrecord headers.

}  // namespace
}  // namespace gsf
