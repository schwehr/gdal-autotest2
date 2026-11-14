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
// Tests GSF file informational queries (src/gsf_info.c). Note: _LARGEFILE64_SOURCE
// is enabled and DISPLAY_SPINNER is disabled in third_party/gsf/BUILD.
//
// See also:
//   https://github.com/schwehr/generic-sensor-format-3/blob/main/src/gsf_info.c
//   https://github.com/schwehr/generic-sensor-format-3/blob/main/src/gsf.h

#include "third_party/gsf/src/gsf.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>  // NOLINT
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gsf/google/util/gsf_internal.h"
#include "third_party/gsf/google/util/temp_file.h"

namespace gsf {
namespace {

struct InfoFnCase {
  std::string_view name;
  int (*fn)(int, int*);
};

constexpr std::array<InfoFnCase, 5> kAllInfoFunctions = {{
    {.name = "gsfFileSupportsRecalculateXYZ",
     .fn = gsfFileSupportsRecalculateXYZ},
    {.name = "gsfFileSupportsRecalculateTPU",
     .fn = gsfFileSupportsRecalculateTPU},
    {.name = "gsfFileSupportsRecalculateNominalDepth",
     .fn = gsfFileSupportsRecalculateNominalDepth},
    {.name = "gsfFileContainsMBAmplitude", .fn = gsfFileContainsMBAmplitude},
    {.name = "gsfFileContainsMBImagery", .fn = gsfFileContainsMBImagery},
}};

constexpr std::array<int, 16> kResonAndEM3RawSensors = {
    GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_RESON_8111_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_RESON_8124_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_RESON_8125_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_RESON_8150_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_RESON_8160_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_RESON_TSERIES_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_EM300_RAW_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_EM1002_RAW_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_EM2000_RAW_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_EM3000_RAW_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_EM120_RAW_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_EM3002_RAW_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_EM3000D_RAW_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_EM3002D_RAW_SPECIFIC,
};

constexpr std::array<int, 4> kEM4Sensors = {
    GSF_SWATH_BATHY_SUBRECORD_EM710_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_EM302_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_EM122_SPECIFIC,
    GSF_SWATH_BATHY_SUBRECORD_ME70BO_SPECIFIC,
};

struct PingSpec {
  int sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC;
  bool with_depth = false;
  bool with_across_track = false;
  bool with_travel_time = false;
  bool with_beam_angle = false;
  bool with_mc_amplitude = false;
  bool with_mr_amplitude = false;
  bool with_sector_number = false;
  bool with_em4_signal_length = false;
  bool with_brb_inten = false;
};

// Uses a monotonically increasing directory name so repeated or reordered test
// runs never collide with cached filenames in gsfFileTable.
TempDir MakeUniqueTempDir() {
  static int dir_counter = 0;
  return TempDir("gsf_info_test_" + std::to_string(dir_counter++));
}

void AppendBigEndianU32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
  out.push_back(static_cast<uint8_t>(value & 0xFFu));
}

void WritePing(int handle, const PingSpec& spec) {
  constexpr short kBeams = 2;
  constexpr char kDefaultNoComp =
      static_cast<char>(GSF_DISABLE_COMPRESSION | GSF_FIELD_SIZE_DEFAULT);

  std::array<double, kBeams> depth = {10.0, 12.0};
  std::array<double, kBeams> across_track = {-5.0, 5.0};
  std::array<double, kBeams> travel_time = {0.02, 0.025};
  std::array<double, kBeams> beam_angle = {-10.0, 10.0};
  // Keep mc_amplitude non-negative because EncodeSignedByteArray casts
  // dtemp to plain `char`, which is unsigned under -funsigned-char.
  std::array<double, kBeams> mc_amplitude = {10.0, 15.0};
  std::array<double, kBeams> mr_amplitude = {15.0, 20.0};
  std::array<unsigned short, kBeams> sector_number = {0, 1};

  gsfRecords rec{};
  std::memset(&rec, 0, sizeof(rec));
  rec.mb_ping.ping_time = {1700000000, 0};
  rec.mb_ping.latitude = 37.5;
  rec.mb_ping.longitude = -122.5;
  rec.mb_ping.number_beams = kBeams;
  rec.mb_ping.center_beam = 1;
  rec.mb_ping.heading = 45.0;
  rec.mb_ping.sensor_id = spec.sensor_id;

  auto attach_array = [&](bool enabled, unsigned int subrecord_id,
                          double precision, auto* ptr, auto& storage) {
    if (!enabled) return;
    *ptr = storage.data();
    ASSERT_EQ(GSF_NORMAL,
              gsfLoadScaleFactor(&rec.mb_ping.scaleFactors, subrecord_id,
                                 kDefaultNoComp, precision, /*offset=*/0));
  };

  ASSERT_NO_FATAL_FAILURE(attach_array(spec.with_depth,
                                       GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                                       0.01, &rec.mb_ping.depth, depth));
  ASSERT_NO_FATAL_FAILURE(attach_array(
      spec.with_across_track, GSF_SWATH_BATHY_SUBRECORD_ACROSS_TRACK_ARRAY,
      0.01, &rec.mb_ping.across_track, across_track));
  ASSERT_NO_FATAL_FAILURE(attach_array(
      spec.with_travel_time, GSF_SWATH_BATHY_SUBRECORD_TRAVEL_TIME_ARRAY,
      0.0001, &rec.mb_ping.travel_time, travel_time));
  ASSERT_NO_FATAL_FAILURE(attach_array(
      spec.with_beam_angle, GSF_SWATH_BATHY_SUBRECORD_BEAM_ANGLE_ARRAY, 0.01,
      &rec.mb_ping.beam_angle, beam_angle));
  ASSERT_NO_FATAL_FAILURE(
      attach_array(spec.with_mc_amplitude,
                   GSF_SWATH_BATHY_SUBRECORD_MEAN_CAL_AMPLITUDE_ARRAY, 0.5,
                   &rec.mb_ping.mc_amplitude, mc_amplitude));
  ASSERT_NO_FATAL_FAILURE(
      attach_array(spec.with_mr_amplitude,
                   GSF_SWATH_BATHY_SUBRECORD_MEAN_REL_AMPLITUDE_ARRAY, 0.5,
                   &rec.mb_ping.mr_amplitude, mr_amplitude));
  ASSERT_NO_FATAL_FAILURE(attach_array(
      spec.with_sector_number, GSF_SWATH_BATHY_SUBRECORD_SECTOR_NUMBER_ARRAY,
      1.0, &rec.mb_ping.sector_number, sector_number));

  // DecodeScaleFactors rejects pings whose scale-factor subrecord has 0 arrays.
  ASSERT_GT(rec.mb_ping.scaleFactors.numArraySubrecords, 0);

  if (spec.with_em4_signal_length) {
    rec.mb_ping.sensor_data.gsfEM4Specific.transmit_sectors = 1;
    rec.mb_ping.sensor_data.gsfEM4Specific.sector[0].signal_length = 0.001;
  }

  gsfBRBIntensity inten{};
  std::memset(&inten, 0, sizeof(inten));
  std::array<unsigned int, 2> beam0_samples = {10, 20};
  std::array<unsigned int, 2> beam1_samples = {30, 40};
  std::array<gsfTimeSeriesIntensity, kBeams> time_series{};
  if (spec.with_brb_inten) {
    time_series[0].sample_count = 2;
    time_series[0].detect_sample = 1;
    time_series[0].samples = beam0_samples.data();
    time_series[1].sample_count = 2;
    time_series[1].detect_sample = 1;
    time_series[1].samples = beam1_samples.data();

    inten.bits_per_sample = 8;
    inten.applied_corrections = GSF_INTENSITY_LINEAR;
    if (spec.sensor_id == GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC) {
      inten.sensor_imagery.gsfReson7100ImagerySpecific = {.size = 66};
    } else if (spec.sensor_id == GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC) {
      inten.sensor_imagery.gsfEM3ImagerySpecific = {.scale = 2};
    }
    inten.time_series = time_series.data();
    rec.mb_ping.brb_inten = &inten;
  }

  gsfDataID id{};
  id.recordID = GSF_RECORD_SWATH_BATHYMETRY_PING;
  ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
}

void WriteSvp(int handle, int num_points) {
  ASSERT_GE(num_points, 1);
  std::vector<double> depths(num_points);
  std::vector<double> speeds(num_points);
  for (int i = 0; i < num_points; ++i) {
    depths[i] = i * 50.0;
    speeds[i] = 1500.0 + i * 5.0;
  }

  gsfRecords rec{};
  rec.svp.observation_time = {1700000000, 0};
  rec.svp.application_time = {1700000000, 0};
  rec.svp.latitude = 37.5;
  rec.svp.longitude = -122.5;
  rec.svp.number_points = num_points;
  rec.svp.depth = depths.data();
  rec.svp.sound_speed = speeds.data();

  gsfDataID id{};
  id.recordID = GSF_RECORD_SOUND_VELOCITY_PROFILE;
  ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
}

void WriteProcessingParams(int handle, int num_params) {
  ASSERT_GE(num_params, 1);
  ASSERT_LE(num_params, GSF_MAX_PROCESSING_PARAMETERS);
  std::vector<std::string> storage;
  storage.reserve(num_params);

  gsfRecords rec{};
  rec.process_parameters.param_time = {1700000000, 0};
  rec.process_parameters.number_parameters = num_params;
  for (int i = 0; i < num_params; ++i) {
    storage.push_back("PARAM_" + std::to_string(i) + "=VALUE");
    rec.process_parameters.param[i] = storage.back().data();
  }

  gsfDataID id{};
  id.recordID = GSF_RECORD_PROCESSING_PARAMETERS;
  ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
}

void WriteAttitude(int handle, int num_measurements) {
  ASSERT_GE(num_measurements, 1);
  std::vector<struct timespec> times(num_measurements);
  std::vector<double> pitch(num_measurements, 0.5);
  std::vector<double> roll(num_measurements, -0.5);
  std::vector<double> heave(num_measurements, 0.1);
  std::vector<double> heading(num_measurements, 90.0);
  for (int i = 0; i < num_measurements; ++i) {
    times[i] = {static_cast<time_t>(1700000000 + i), 0};
  }

  gsfRecords rec{};
  rec.attitude.num_measurements = static_cast<short>(num_measurements);
  rec.attitude.attitude_time = times.data();
  rec.attitude.pitch = pitch.data();
  rec.attitude.roll = roll.data();
  rec.attitude.heave = heave.data();
  rec.attitude.heading = heading.data();

  gsfDataID id{};
  id.recordID = GSF_RECORD_ATTITUDE;
  ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
}

void WriteComment(int handle, std::string_view text, int checksum_flag = 0) {
  std::string owned_text(text);
  gsfRecords rec{};
  rec.comment.comment_time = {1700000000, 0};
  rec.comment.comment_length = static_cast<int>(owned_text.size());
  rec.comment.comment = owned_text.data();

  gsfDataID id{};
  id.checksumFlag = checksum_flag;
  id.recordID = GSF_RECORD_COMMENT;
  ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
}

void WriteAllNonRelevantRecords(int handle) {
  // 1. GSF_RECORD_NAVIGATION_ERROR
  {
    gsfRecords rec{};
    rec.nav_error.nav_error_time = {1700000000, 0};
    rec.nav_error.record_id = GSF_RECORD_SWATH_BATHYMETRY_PING;
    rec.nav_error.latitude_error = 1.5;
    rec.nav_error.longitude_error = 2.5;
    gsfDataID id{};
    id.recordID = GSF_RECORD_NAVIGATION_ERROR;
    ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
  }
  // 2. GSF_RECORD_SWATH_BATHY_SUMMARY
  {
    gsfRecords rec{};
    rec.summary.start_time = {1700000000, 0};
    rec.summary.end_time = {1700000010, 0};
    rec.summary.min_latitude = 37.0;
    rec.summary.max_latitude = 38.0;
    rec.summary.min_longitude = -123.0;
    rec.summary.max_longitude = -122.0;
    rec.summary.min_depth = 10.0;
    rec.summary.max_depth = 50.0;
    gsfDataID id{};
    id.recordID = GSF_RECORD_SWATH_BATHY_SUMMARY;
    ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
  }
  // 3. GSF_RECORD_HISTORY
  {
    constexpr char kHost[] = "testhost";
    constexpr char kOperator[] = "tester";
    char cmd[] = "gsf_info_test";
    char hist_comment[] = "non-relevant history";
    gsfRecords rec{};
    rec.history.history_time = {1700000000, 0};
    std::memcpy(rec.history.host_name, kHost, sizeof(kHost));
    std::memcpy(rec.history.operator_name, kOperator, sizeof(kOperator));
    rec.history.command_line = cmd;
    rec.history.comment = hist_comment;
    gsfDataID id{};
    id.recordID = GSF_RECORD_HISTORY;
    ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
  }
  // 4. GSF_RECORD_SENSOR_PARAMETERS
  {
    char param[] = "MOUNT_ROLL=0.0";
    gsfRecords rec{};
    rec.sensor_parameters.param_time = {1700000000, 0};
    rec.sensor_parameters.number_parameters = 1;
    rec.sensor_parameters.param[0] = param;
    gsfDataID id{};
    id.recordID = GSF_RECORD_SENSOR_PARAMETERS;
    ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
  }
  // 5. GSF_RECORD_COMMENT
  ASSERT_NO_FATAL_FAILURE(WriteComment(handle, "non-relevant comment"));
  // 6. GSF_RECORD_HV_NAVIGATION_ERROR (hits default: branch in switch)
  {
    char pos_type[] = GSF_POS_TYPE_GPSU;
    gsfRecords rec{};
    rec.hv_nav_error.nav_error_time = {1700000000, 0};
    rec.hv_nav_error.record_id = GSF_RECORD_SWATH_BATHYMETRY_PING;
    rec.hv_nav_error.horizontal_error = 1.0;
    rec.hv_nav_error.vertical_error = 0.5;
    rec.hv_nav_error.SEP_uncertainty = 0.2;
    rec.hv_nav_error.position_type = pos_type;
    gsfDataID id{};
    id.recordID = GSF_RECORD_HV_NAVIGATION_ERROR;
    ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
  }
}

void ExpectRewoundToHeader(int handle) {
  gsfDataID id{};
  gsfRecords rec{};
  ASSERT_GT(gsfRead(handle, GSF_NEXT_RECORD, &id, &rec, nullptr, 0), 0);
  EXPECT_EQ(GSF_RECORD_HEADER, id.recordID);
}

// Frees per-beam BRB intensity sample buffers (which gsfFree misses because
// gsfFileTable[slot].rec.mb_ping.number_beams stays 0) and resets arraySize so
// a subsequent GSF_READONLY open frees samplesArraySize cleanly.
void ResetImageryArraySizeOnClose(int handle) {
  if (handle < 1 || handle > GSF_MAX_OPEN_FILES) return;
  if (gsfSeek(handle, GSF_REWIND) == GSF_NORMAL) {
    gsfDataID id{};
    gsfRecords rec{};
    while (gsfRead(handle, GSF_NEXT_RECORD, &id, &rec, nullptr, 0) > 0) {
      if (id.recordID == GSF_RECORD_SWATH_BATHYMETRY_PING &&
          rec.mb_ping.brb_inten != nullptr &&
          rec.mb_ping.brb_inten->time_series != nullptr) {
        for (int i = 0; i < rec.mb_ping.number_beams; ++i) {
          std::free(rec.mb_ping.brb_inten->time_series[i].samples);
          rec.mb_ping.brb_inten->time_series[i].samples = nullptr;
        }
      }
    }
  }
  constexpr int kIdx = GSF_SWATH_BATHY_SUBRECORD_INTENSITY_SERIES_ARRAY - 1;
  arraySize[handle - 1][kIdx] = 0;
}

template <typename WriterFn, typename VerifierFn>
void WithCreatedAndReadonlyFile(const std::filesystem::path& file_path,
                                int open_mode, bool has_imagery,
                                WriterFn&& writer_fn,
                                VerifierFn&& verifier_fn) {
  const std::string path = file_path.string();
  int write_handle = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_CREATE, &write_handle));
  absl::Cleanup close_writer = [&] {
    if (write_handle > 0) {
      EXPECT_EQ(GSF_NORMAL, gsfClose(write_handle));
    }
  };
  ASSERT_NO_FATAL_FAILURE(writer_fn(write_handle));
  const int close_ret = gsfClose(write_handle);
  write_handle = 0;
  std::move(close_writer).Cancel();
  ASSERT_EQ(GSF_NORMAL, close_ret);

  int read_handle = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), open_mode, &read_handle));
  absl::Cleanup close_reader = [&] {
    if (read_handle > 0) {
      if (has_imagery) {
        ResetImageryArraySizeOnClose(read_handle);
      }
      EXPECT_EQ(GSF_NORMAL, gsfClose(read_handle));
    }
  };
  ASSERT_NO_FATAL_FAILURE(verifier_fn(read_handle));
}

TEST(GsfInfoTest, BadHandleFailsWithFileSeekError) {
  constexpr std::array<int, 3> kBadHandles = {-1, 0, GSF_MAX_OPEN_FILES + 1};
  for (int bad_handle : kBadHandles) {
    for (const auto& info_fn : kAllInfoFunctions) {
      SCOPED_TRACE(info_fn.name);
      int status = -1;
      EXPECT_EQ(-1, info_fn.fn(bad_handle, &status));
      EXPECT_EQ(GSF_FILE_SEEK_ERROR, gsfIntError());
      EXPECT_STREQ("GSF Error: File seek failed", gsfStringError());
      EXPECT_EQ(0, status);
    }
  }
}

TEST(GsfInfoTest, HeaderOnlyFileReturnsZeroStatusAndRewinds) {
  TempDir dir = MakeUniqueTempDir();
  ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
      dir.file("header_only.gsf"), GSF_READONLY, /*has_imagery=*/false,
      [](int /*write_handle*/) {},
      [](int handle) {
        for (const auto& info_fn : kAllInfoFunctions) {
          SCOPED_TRACE(info_fn.name);
          int status = 1;
          EXPECT_EQ(GSF_NORMAL, info_fn.fn(handle, &status));
          EXPECT_EQ(0, status);
          ASSERT_NO_FATAL_FAILURE(ExpectRewoundToHeader(handle));
        }
      }));
}

TEST(GsfInfoTest, CorruptRecordSizeFails) {
  TempDir dir = MakeUniqueTempDir();
  const std::string path = dir.file("corrupt_size.gsf").string();

  // 1. Write a valid GSF header so gsfOpen(GSF_READONLY) succeeds.
  int write_handle = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_CREATE, &write_handle));
  ASSERT_EQ(GSF_NORMAL, gsfClose(write_handle));

  // 2. Append an 8-byte record prefix with dataSize=4 (<= 8 -> size error).
  std::vector<uint8_t> corrupt_prefix;
  AppendBigEndianU32(corrupt_prefix, 4);
  AppendBigEndianU32(corrupt_prefix, GSF_RECORD_COMMENT);
  {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    ASSERT_TRUE(out.good());
    out.write(reinterpret_cast<const char*>(corrupt_prefix.data()),
              static_cast<std::streamsize>(corrupt_prefix.size()));
    ASSERT_TRUE(out.good());
  }

  // 3. All 5 info functions must fail with -1 on the non-EOF read error.
  int read_handle = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_READONLY, &read_handle));
  absl::Cleanup close_reader = [&] {
    EXPECT_EQ(GSF_NORMAL, gsfClose(read_handle));
  };

  for (const auto& info_fn : kAllInfoFunctions) {
    SCOPED_TRACE(info_fn.name);
    int status = 1;
    EXPECT_EQ(-1, info_fn.fn(read_handle, &status));
    EXPECT_EQ(GSF_RECORD_SIZE_ERROR, gsfIntError());
    EXPECT_EQ(0, status);
  }
}

TEST(GsfInfoTest, ChecksumFailureFails) {
  TempDir dir = MakeUniqueTempDir();
  const std::string path = dir.file("bad_checksum.gsf").string();

  // 1. Create a file containing a checksummed comment record.
  int write_handle = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_CREATE, &write_handle));
  absl::Cleanup close_writer = [&] {
    if (write_handle > 0) {
      EXPECT_EQ(GSF_NORMAL, gsfClose(write_handle));
    }
  };
  ASSERT_NO_FATAL_FAILURE(WriteComment(
      write_handle, "checksummed comment payload", /*checksum_flag=*/1));
  const int close_ret = gsfClose(write_handle);
  write_handle = 0;
  std::move(close_writer).Cancel();
  ASSERT_EQ(GSF_NORMAL, close_ret);

  // 2. Corrupt the payload while keeping the header and record size intact.
  std::vector<uint8_t> bytes;
  {
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good());
    bytes.assign(std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
  }
  ASSERT_GT(bytes.size(), 32u);
  bytes[bytes.size() - 5] ^= 0xFFu;
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good());
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(out.good());
  }

  // 3. All 5 info functions must fail with GSF_CHECKSUM_FAILURE.
  int read_handle = 0;
  ASSERT_EQ(GSF_NORMAL, gsfOpen(path.c_str(), GSF_READONLY, &read_handle));
  absl::Cleanup close_reader = [&] {
    EXPECT_EQ(GSF_NORMAL, gsfClose(read_handle));
  };

  for (const auto& info_fn : kAllInfoFunctions) {
    SCOPED_TRACE(info_fn.name);
    int status = 1;
    EXPECT_EQ(-1, info_fn.fn(read_handle, &status));
    EXPECT_EQ(GSF_CHECKSUM_FAILURE, gsfIntError());
    EXPECT_EQ(0, status);
  }
}

TEST(GsfInfoTest, NonRelevantAndSinglePointRecordsReturnZero) {
  TempDir dir = MakeUniqueTempDir();
  ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
      dir.file("non_relevant.gsf"), GSF_READONLY, /*has_imagery=*/false,
      [](int handle) {
        ASSERT_NO_FATAL_FAILURE(WriteAllNonRelevantRecords(handle));
        // Boundary checks: > 1 required for SVP, ProcessingParams, Attitude.
        ASSERT_NO_FATAL_FAILURE(WriteSvp(handle, /*num_points=*/1));
        ASSERT_NO_FATAL_FAILURE(
            WriteProcessingParams(handle, /*num_params=*/1));
        ASSERT_NO_FATAL_FAILURE(WriteAttitude(handle, /*num_measurements=*/1));
        // Ping with only across_track (>= 1 array required for scale factors),
        // lacking travel_time, beam_angle, depth, amplitude, and imagery.
        ASSERT_NO_FATAL_FAILURE(WritePing(
            handle, {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC,
                     .with_across_track = true}));
      },
      [](int handle) {
        for (const auto& info_fn : kAllInfoFunctions) {
          SCOPED_TRACE(info_fn.name);
          int status = 1;
          EXPECT_EQ(GSF_NORMAL, info_fn.fn(handle, &status));
          EXPECT_EQ(0, status);
          ASSERT_NO_FATAL_FAILURE(ExpectRewoundToHeader(handle));
        }
      }));
}

TEST(GsfInfoTest, HundredRecordScanCapBoundary) {
  TempDir dir = MakeUniqueTempDir();

  // 1. Exactly 100 records (header + 95 comments + ping + SVP + params +
  // attitude): all 5 info functions succeed with status 1.
  ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
      dir.file("exact_100.gsf"), GSF_READONLY, /*has_imagery=*/true,
      [](int handle) {
        for (int i = 0; i < 95; ++i) {
          ASSERT_NO_FATAL_FAILURE(WriteComment(handle, "padding"));
        }
        ASSERT_NO_FATAL_FAILURE(WritePing(
            handle, {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC,
                     .with_depth = true,
                     .with_across_track = true,
                     .with_travel_time = true,
                     .with_beam_angle = true,
                     .with_mc_amplitude = true,
                     .with_brb_inten = true}));
        ASSERT_NO_FATAL_FAILURE(WriteSvp(handle, /*num_points=*/2));
        ASSERT_NO_FATAL_FAILURE(
            WriteProcessingParams(handle, /*num_params=*/2));
        ASSERT_NO_FATAL_FAILURE(WriteAttitude(handle, /*num_measurements=*/2));
      },
      [](int handle) {
        for (const auto& info_fn : kAllInfoFunctions) {
          SCOPED_TRACE(info_fn.name);
          int status = 0;
          EXPECT_EQ(GSF_NORMAL, info_fn.fn(handle, &status));
          EXPECT_EQ(1, status);
          ASSERT_NO_FATAL_FAILURE(ExpectRewoundToHeader(handle));
        }
      }));

  // 2. Record 0 is the header; records 1..100 are comments, pushing the
  // valid ping/SVP/params/attitude past the 100-record scan cap.
  ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
      dir.file("over_100.gsf"), GSF_READONLY, /*has_imagery=*/false,
      [](int handle) {
        for (int i = 0; i < 100; ++i) {
          ASSERT_NO_FATAL_FAILURE(WriteComment(handle, "padding"));
        }
        ASSERT_NO_FATAL_FAILURE(WritePing(
            handle, {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                     .with_depth = true,
                     .with_across_track = true,
                     .with_travel_time = true,
                     .with_beam_angle = true,
                     .with_mc_amplitude = true,
                     .with_brb_inten = true}));
        ASSERT_NO_FATAL_FAILURE(WriteSvp(handle, /*num_points=*/2));
        ASSERT_NO_FATAL_FAILURE(
            WriteProcessingParams(handle, /*num_params=*/2));
        ASSERT_NO_FATAL_FAILURE(WriteAttitude(handle, /*num_measurements=*/2));
      },
      [](int handle) {
        for (const auto& info_fn : kAllInfoFunctions) {
          SCOPED_TRACE(info_fn.name);
          // Seek to EOF first to verify the function rewinds on entry as well.
          ASSERT_EQ(GSF_NORMAL, gsfSeek(handle, GSF_END_OF_FILE));
          int status = 1;
          EXPECT_EQ(GSF_NORMAL, info_fn.fn(handle, &status));
          EXPECT_EQ(0, status);
          ASSERT_NO_FATAL_FAILURE(ExpectRewoundToHeader(handle));
        }
      }));
}

TEST(GsfInfoTest, RecalculateXYZSupportsAllResonAndEM3RawSensors) {
  TempDir dir = MakeUniqueTempDir();
  for (std::size_t i = 0; i < kResonAndEM3RawSensors.size(); ++i) {
    const int sensor_id = kResonAndEM3RawSensors[i];
    SCOPED_TRACE(sensor_id);
    ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
        dir.file("xyz_sensor_" + std::to_string(i) + ".gsf"), GSF_READONLY,
        /*has_imagery=*/false,
        [sensor_id](int handle) {
          ASSERT_NO_FATAL_FAILURE(WritePing(handle, {.sensor_id = sensor_id,
                                                     .with_travel_time = true,
                                                     .with_beam_angle = true}));
          ASSERT_NO_FATAL_FAILURE(WriteSvp(handle, /*num_points=*/2));
          ASSERT_NO_FATAL_FAILURE(
              WriteProcessingParams(handle, /*num_params=*/2));
          ASSERT_NO_FATAL_FAILURE(
              WriteAttitude(handle, /*num_measurements=*/2));
        },
        [](int handle) {
          int status = 0;
          EXPECT_EQ(GSF_NORMAL, gsfFileSupportsRecalculateXYZ(handle, &status));
          EXPECT_EQ(1, status);
          ASSERT_NO_FATAL_FAILURE(ExpectRewoundToHeader(handle));
        }));
  }
}

TEST(GsfInfoTest, RecalculateXYZRequiresSectorNumberForEM4Sensors) {
  TempDir dir = MakeUniqueTempDir();
  for (std::size_t i = 0; i < kEM4Sensors.size(); ++i) {
    const int sensor_id = kEM4Sensors[i];
    SCOPED_TRACE(sensor_id);

    // 1. EM4 without sector_number must return status 0.
    ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
        dir.file("xyz_em4_no_sector_" + std::to_string(i) + ".gsf"),
        GSF_READONLY, /*has_imagery=*/false,
        [sensor_id](int handle) {
          ASSERT_NO_FATAL_FAILURE(WritePing(handle, {.sensor_id = sensor_id,
                                                     .with_travel_time = true,
                                                     .with_beam_angle = true}));
          ASSERT_NO_FATAL_FAILURE(WriteSvp(handle, /*num_points=*/2));
          ASSERT_NO_FATAL_FAILURE(
              WriteProcessingParams(handle, /*num_params=*/2));
          ASSERT_NO_FATAL_FAILURE(
              WriteAttitude(handle, /*num_measurements=*/2));
        },
        [](int handle) {
          int status = 1;
          EXPECT_EQ(GSF_NORMAL, gsfFileSupportsRecalculateXYZ(handle, &status));
          EXPECT_EQ(0, status);
        }));

    // 2. EM4 with sector_number must return status 1.
    ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
        dir.file("xyz_em4_with_sector_" + std::to_string(i) + ".gsf"),
        GSF_READONLY, /*has_imagery=*/false,
        [sensor_id](int handle) {
          ASSERT_NO_FATAL_FAILURE(
              WritePing(handle, {.sensor_id = sensor_id,
                                 .with_travel_time = true,
                                 .with_beam_angle = true,
                                 .with_sector_number = true}));
          ASSERT_NO_FATAL_FAILURE(WriteSvp(handle, /*num_points=*/2));
          ASSERT_NO_FATAL_FAILURE(
              WriteProcessingParams(handle, /*num_params=*/2));
          ASSERT_NO_FATAL_FAILURE(
              WriteAttitude(handle, /*num_measurements=*/2));
        },
        [](int handle) {
          int status = 0;
          EXPECT_EQ(GSF_NORMAL, gsfFileSupportsRecalculateXYZ(handle, &status));
          EXPECT_EQ(1, status);
        }));
  }
}

TEST(GsfInfoTest, RecalculateXYZReturnsZeroWhenAnyPrerequisiteMissing) {
  TempDir dir = MakeUniqueTempDir();

  struct XyzNegativeCase {
    std::string_view tag;
    PingSpec ping;
    int svp_points;
    int param_count;
    int attitude_count;
  };
  constexpr std::array<XyzNegativeCase, 6> kCases = {{
      {.tag = "unsupported_sensor",
       .ping = {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_SEABEAM_SPECIFIC,
                .with_travel_time = true,
                .with_beam_angle = true},
       .svp_points = 2,
       .param_count = 2,
       .attitude_count = 2},
      {.tag = "missing_beam_angle",
       .ping = {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                .with_travel_time = true},
       .svp_points = 2,
       .param_count = 2,
       .attitude_count = 2},
      {.tag = "missing_travel_time",
       .ping = {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                .with_beam_angle = true},
       .svp_points = 2,
       .param_count = 2,
       .attitude_count = 2},
      {.tag = "insufficient_svp",
       .ping = {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                .with_travel_time = true,
                .with_beam_angle = true},
       .svp_points = 1,
       .param_count = 2,
       .attitude_count = 2},
      {.tag = "insufficient_params",
       .ping = {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                .with_travel_time = true,
                .with_beam_angle = true},
       .svp_points = 2,
       .param_count = 1,
       .attitude_count = 2},
      {.tag = "insufficient_attitude",
       .ping = {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                .with_travel_time = true,
                .with_beam_angle = true},
       .svp_points = 2,
       .param_count = 2,
       .attitude_count = 1},
  }};

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.tag);
    ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
        dir.file("xyz_neg_" + std::string(tc.tag) + ".gsf"), GSF_READONLY,
        /*has_imagery=*/false,
        [&](int handle) {
          ASSERT_NO_FATAL_FAILURE(WritePing(handle, tc.ping));
          ASSERT_NO_FATAL_FAILURE(WriteSvp(handle, tc.svp_points));
          ASSERT_NO_FATAL_FAILURE(
              WriteProcessingParams(handle, tc.param_count));
          ASSERT_NO_FATAL_FAILURE(WriteAttitude(handle, tc.attitude_count));
        },
        [](int handle) {
          int status = 1;
          EXPECT_EQ(GSF_NORMAL, gsfFileSupportsRecalculateXYZ(handle, &status));
          EXPECT_EQ(0, status);
        }));
  }
}

TEST(GsfInfoTest, RecalculateTPUSupportsNonEM4AndEM4Sensors) {
  TempDir dir = MakeUniqueTempDir();

  // 1. Non-EM4 sensor with depth, across_track, SVP(>1), and params(>1).
  ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
      dir.file("tpu_non_em4.gsf"), GSF_READONLY, /*has_imagery=*/false,
      [](int handle) {
        ASSERT_NO_FATAL_FAILURE(WriteAllNonRelevantRecords(handle));
        ASSERT_NO_FATAL_FAILURE(WriteAttitude(handle, /*num_measurements=*/2));
        ASSERT_NO_FATAL_FAILURE(WritePing(
            handle, {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                     .with_depth = true,
                     .with_across_track = true}));
        ASSERT_NO_FATAL_FAILURE(WriteSvp(handle, /*num_points=*/2));
        ASSERT_NO_FATAL_FAILURE(
            WriteProcessingParams(handle, /*num_params=*/2));
      },
      [](int handle) {
        int status = 0;
        EXPECT_EQ(GSF_NORMAL, gsfFileSupportsRecalculateTPU(handle, &status));
        EXPECT_EQ(1, status);
        ASSERT_NO_FATAL_FAILURE(ExpectRewoundToHeader(handle));
      }));

  // 2. EM4 sensors require both sector_number and sector[0].signal_length > 0.
  for (std::size_t i = 0; i < kEM4Sensors.size(); ++i) {
    const int sensor_id = kEM4Sensors[i];
    SCOPED_TRACE(sensor_id);

    ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
        dir.file("tpu_em4_no_sector_" + std::to_string(i) + ".gsf"),
        GSF_READONLY, /*has_imagery=*/false,
        [sensor_id](int handle) {
          ASSERT_NO_FATAL_FAILURE(
              WritePing(handle, {.sensor_id = sensor_id,
                                 .with_depth = true,
                                 .with_across_track = true,
                                 .with_em4_signal_length = true}));
          ASSERT_NO_FATAL_FAILURE(WriteSvp(handle, /*num_points=*/2));
          ASSERT_NO_FATAL_FAILURE(
              WriteProcessingParams(handle, /*num_params=*/2));
        },
        [](int handle) {
          int status = 1;
          EXPECT_EQ(GSF_NORMAL, gsfFileSupportsRecalculateTPU(handle, &status));
          EXPECT_EQ(0, status);
        }));

    ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
        dir.file("tpu_em4_zero_sig_" + std::to_string(i) + ".gsf"),
        GSF_READONLY, /*has_imagery=*/false,
        [sensor_id](int handle) {
          ASSERT_NO_FATAL_FAILURE(
              WritePing(handle, {.sensor_id = sensor_id,
                                 .with_depth = true,
                                 .with_across_track = true,
                                 .with_sector_number = true,
                                 .with_em4_signal_length = false}));
          ASSERT_NO_FATAL_FAILURE(WriteSvp(handle, /*num_points=*/2));
          ASSERT_NO_FATAL_FAILURE(
              WriteProcessingParams(handle, /*num_params=*/2));
        },
        [](int handle) {
          int status = 1;
          EXPECT_EQ(GSF_NORMAL, gsfFileSupportsRecalculateTPU(handle, &status));
          EXPECT_EQ(0, status);
        }));

    ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
        dir.file("tpu_em4_ok_" + std::to_string(i) + ".gsf"), GSF_READONLY,
        /*has_imagery=*/false,
        [sensor_id](int handle) {
          ASSERT_NO_FATAL_FAILURE(
              WritePing(handle, {.sensor_id = sensor_id,
                                 .with_depth = true,
                                 .with_across_track = true,
                                 .with_sector_number = true,
                                 .with_em4_signal_length = true}));
          ASSERT_NO_FATAL_FAILURE(WriteSvp(handle, /*num_points=*/2));
          ASSERT_NO_FATAL_FAILURE(
              WriteProcessingParams(handle, /*num_params=*/2));
        },
        [](int handle) {
          int status = 0;
          EXPECT_EQ(GSF_NORMAL, gsfFileSupportsRecalculateTPU(handle, &status));
          EXPECT_EQ(1, status);
        }));
  }
}

TEST(GsfInfoTest, RecalculateTPUReturnsZeroWhenPrerequisitesMissing) {
  TempDir dir = MakeUniqueTempDir();

  struct TpuNegativeCase {
    std::string_view tag;
    PingSpec ping;
    int svp_points;
    int param_count;
  };
  constexpr std::array<TpuNegativeCase, 4> kCases = {{
      {.tag = "missing_across_track",
       .ping = {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                .with_depth = true},
       .svp_points = 2,
       .param_count = 2},
      {.tag = "missing_depth",
       .ping = {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                .with_across_track = true},
       .svp_points = 2,
       .param_count = 2},
      {.tag = "insufficient_svp",
       .ping = {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                .with_depth = true,
                .with_across_track = true},
       .svp_points = 1,
       .param_count = 2},
      {.tag = "insufficient_params",
       .ping = {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                .with_depth = true,
                .with_across_track = true},
       .svp_points = 2,
       .param_count = 1},
  }};

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.tag);
    ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
        dir.file("tpu_neg_" + std::string(tc.tag) + ".gsf"), GSF_READONLY,
        /*has_imagery=*/false,
        [&](int handle) {
          ASSERT_NO_FATAL_FAILURE(WritePing(handle, tc.ping));
          ASSERT_NO_FATAL_FAILURE(WriteSvp(handle, tc.svp_points));
          ASSERT_NO_FATAL_FAILURE(
              WriteProcessingParams(handle, tc.param_count));
        },
        [](int handle) {
          int status = 1;
          EXPECT_EQ(GSF_NORMAL, gsfFileSupportsRecalculateTPU(handle, &status));
          EXPECT_EQ(0, status);
        }));
  }
}

TEST(GsfInfoTest, RecalculateNominalDepthChecksDepthSvpAndParams) {
  TempDir dir = MakeUniqueTempDir();

  // 1. Positive path: travel_time + depth + SVP(>1) + ProcessingParams(>1).
  ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
      dir.file("nom_depth_ok.gsf"), GSF_READONLY, /*has_imagery=*/false,
      [](int handle) {
        ASSERT_NO_FATAL_FAILURE(WriteAllNonRelevantRecords(handle));
        ASSERT_NO_FATAL_FAILURE(WriteAttitude(handle, /*num_measurements=*/2));
        ASSERT_NO_FATAL_FAILURE(WritePing(
            handle, {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                     .with_depth = true,
                     .with_travel_time = true}));
        ASSERT_NO_FATAL_FAILURE(WriteSvp(handle, /*num_points=*/2));
        ASSERT_NO_FATAL_FAILURE(
            WriteProcessingParams(handle, /*num_params=*/2));
      },
      [](int handle) {
        int status = 0;
        EXPECT_EQ(GSF_NORMAL,
                  gsfFileSupportsRecalculateNominalDepth(handle, &status));
        EXPECT_EQ(1, status);
        ASSERT_NO_FATAL_FAILURE(ExpectRewoundToHeader(handle));
      }));

  // 2. Negative paths: missing depth, SVP(1), or ProcessingParams(1).
  struct NomDepthNegativeCase {
    std::string_view tag;
    PingSpec ping;
    int svp_points;
    int param_count;
  };
  constexpr std::array<NomDepthNegativeCase, 3> kNegCases = {{
      {.tag = "missing_depth",
       .ping = {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                .with_travel_time = true},
       .svp_points = 2,
       .param_count = 2},
      {.tag = "insufficient_svp",
       .ping = {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                .with_depth = true,
                .with_travel_time = true},
       .svp_points = 1,
       .param_count = 2},
      {.tag = "insufficient_params",
       .ping = {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                .with_depth = true,
                .with_travel_time = true},
       .svp_points = 2,
       .param_count = 1},
  }};

  for (const auto& tc : kNegCases) {
    SCOPED_TRACE(tc.tag);
    ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
        dir.file("nom_depth_neg_" + std::string(tc.tag) + ".gsf"), GSF_READONLY,
        /*has_imagery=*/false,
        [&](int handle) {
          ASSERT_NO_FATAL_FAILURE(WritePing(handle, tc.ping));
          ASSERT_NO_FATAL_FAILURE(WriteSvp(handle, tc.svp_points));
          ASSERT_NO_FATAL_FAILURE(
              WriteProcessingParams(handle, tc.param_count));
        },
        [](int handle) {
          int status = 1;
          EXPECT_EQ(GSF_NORMAL,
                    gsfFileSupportsRecalculateNominalDepth(handle, &status));
          EXPECT_EQ(0, status);
        }));
  }
}

TEST(GsfInfoTest, ContainsMBAmplitudeDetectsCalibratedAndRelativeAmplitude) {
  TempDir dir = MakeUniqueTempDir();

  struct AmpCase {
    std::string_view tag;
    bool with_mc;
    bool with_mr;
    bool add_second_ping_with_mc;
    int expected_status;
  };
  constexpr std::array<AmpCase, 4> kCases = {{
      {.tag = "mc_amplitude",
       .with_mc = true,
       .with_mr = false,
       .add_second_ping_with_mc = false,
       .expected_status = 1},
      {.tag = "mr_amplitude",
       .with_mc = false,
       .with_mr = true,
       .add_second_ping_with_mc = false,
       .expected_status = 1},
      {.tag = "neither_amplitude",
       .with_mc = false,
       .with_mr = false,
       .add_second_ping_with_mc = false,
       .expected_status = 0},
      {.tag = "stops_at_first_ping",
       .with_mc = false,
       .with_mr = false,
       .add_second_ping_with_mc = true,
       .expected_status = 0},
  }};

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.tag);
    ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
        dir.file("amp_" + std::string(tc.tag) + ".gsf"), GSF_READONLY,
        /*has_imagery=*/false,
        [&](int handle) {
          ASSERT_NO_FATAL_FAILURE(WriteComment(handle, "pre-ping record"));
          ASSERT_NO_FATAL_FAILURE(WritePing(
              handle,
              {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
               .with_depth = true,
               .with_mc_amplitude = tc.with_mc,
               .with_mr_amplitude = tc.with_mr}));
          if (tc.add_second_ping_with_mc) {
            ASSERT_NO_FATAL_FAILURE(WritePing(
                handle,
                {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC,
                 .with_depth = true,
                 .with_mc_amplitude = true}));
          }
        },
        [&](int handle) {
          int status = -1;
          EXPECT_EQ(GSF_NORMAL, gsfFileContainsMBAmplitude(handle, &status));
          EXPECT_EQ(tc.expected_status, status);
          ASSERT_NO_FATAL_FAILURE(ExpectRewoundToHeader(handle));
        }));
  }
}

TEST(GsfInfoTest, ContainsMBImageryDetectsTimeSeriesAndStopsAtFirstPing) {
  TempDir dir = MakeUniqueTempDir();

  // 1. Reson 7125 ping with brb_inten -> status 1.
  ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
      dir.file("imagery_reson7125.gsf"), GSF_READONLY, /*has_imagery=*/true,
      [](int handle) {
        ASSERT_NO_FATAL_FAILURE(WriteComment(handle, "pre-ping record"));
        ASSERT_NO_FATAL_FAILURE(WritePing(
            handle, {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC,
                     .with_depth = true,
                     .with_brb_inten = true}));
      },
      [](int handle) {
        int status = 0;
        EXPECT_EQ(GSF_NORMAL, gsfFileContainsMBImagery(handle, &status));
        EXPECT_EQ(1, status);
        ASSERT_NO_FATAL_FAILURE(ExpectRewoundToHeader(handle));
      }));

  // 2. EM3000 ping with brb_inten -> status 1 (verifies slot reuse cleanup).
  ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
      dir.file("imagery_em3000.gsf"), GSF_READONLY, /*has_imagery=*/true,
      [](int handle) {
        ASSERT_NO_FATAL_FAILURE(WritePing(
            handle, {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_EM3000_SPECIFIC,
                     .with_depth = true,
                     .with_brb_inten = true}));
      },
      [](int handle) {
        int status = 0;
        EXPECT_EQ(GSF_NORMAL, gsfFileContainsMBImagery(handle, &status));
        EXPECT_EQ(1, status);
        ASSERT_NO_FATAL_FAILURE(ExpectRewoundToHeader(handle));
      }));

  // 3. First ping lacks brb_inten while second ping has it -> stops at first
  // ping and returns status 0 without decoding the second ping's brb_inten.
  ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
      dir.file("imagery_stops_at_first.gsf"), GSF_READONLY,
      /*has_imagery=*/false,
      [](int handle) {
        ASSERT_NO_FATAL_FAILURE(WritePing(
            handle, {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC,
                     .with_depth = true}));
        ASSERT_NO_FATAL_FAILURE(WritePing(
            handle, {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC,
                     .with_depth = true,
                     .with_brb_inten = true}));
      },
      [](int handle) {
        int status = 1;
        EXPECT_EQ(GSF_NORMAL, gsfFileContainsMBImagery(handle, &status));
        EXPECT_EQ(0, status);
        ASSERT_NO_FATAL_FAILURE(ExpectRewoundToHeader(handle));
      }));
}

TEST(GsfInfoTest, IndexedReadonlyFileWorksWithAllInfoFunctions) {
  TempDir dir = MakeUniqueTempDir();
  ScopedEnvVar gsftmpdir("GSFTMPDIR", dir.string());

  ASSERT_NO_FATAL_FAILURE(WithCreatedAndReadonlyFile(
      dir.file("indexed.gsf"), GSF_READONLY_INDEX, /*has_imagery=*/true,
      [](int handle) {
        ASSERT_NO_FATAL_FAILURE(WritePing(
            handle, {.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_7125_SPECIFIC,
                     .with_depth = true,
                     .with_across_track = true,
                     .with_travel_time = true,
                     .with_beam_angle = true,
                     .with_mc_amplitude = true,
                     .with_brb_inten = true}));
        ASSERT_NO_FATAL_FAILURE(WriteSvp(handle, /*num_points=*/2));
        ASSERT_NO_FATAL_FAILURE(
            WriteProcessingParams(handle, /*num_params=*/2));
        ASSERT_NO_FATAL_FAILURE(WriteAttitude(handle, /*num_measurements=*/2));
      },
      [](int handle) {
        for (const auto& info_fn : kAllInfoFunctions) {
          SCOPED_TRACE(info_fn.name);
          int status = 0;
          EXPECT_EQ(GSF_NORMAL, info_fn.fn(handle, &status));
          EXPECT_EQ(1, status);
          ASSERT_NO_FATAL_FAILURE(ExpectRewoundToHeader(handle));
        }
      }));
}

// TODO(schwehr): Remaining ~7.7% uncovered lines in src/gsf_info.c are second
// gsfSeek(handle, GSF_REWIND) failure branches after an initial rewind on the
// same handle succeeded.

}  // namespace
}  // namespace gsf
