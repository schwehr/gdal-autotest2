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
// Tests GSF index file creation, reopening, appending, and byte swapping
// (src/gsf_indx.c). Compiled in google3 with _LARGEFILE64_SOURCE and
// DISPLAY_SPINNER undefined.
//
// See also:
//   https://github.com/schwehr/generic-sensor-format-3/blob/main/src/gsf_indx.c
//   https://github.com/schwehr/generic-sensor-format-3/blob/main/src/gsf_indx.h

#include "third_party/gsf/src/gsf.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>  // NOLINT
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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

static_assert(sizeof(short) == 2);
static_assert(sizeof(int) == 4);
static_assert(sizeof(long long) == 8);
static_assert(sizeof(INDEX_REC) == 16);
static_assert(std::endian::native == std::endian::little);

// Trivially destructible ring buffer for gsf_register_progress_callback tests
// (avoids exit-time destructors and keeps the C callback stateless).
struct ProgressTracker {
  static constexpr int kMaxEvents = 512;
  int count = 0;
  int states[kMaxEvents]{};
  int percents[kMaxEvents]{};
};

ProgressTracker g_progress_tracker;

void RecordProgress(int state, int percent) {
  if (g_progress_tracker.count < ProgressTracker::kMaxEvents) {
    const int idx = g_progress_tracker.count++;
    g_progress_tracker.states[idx] = state;
    g_progress_tracker.percents[idx] = percent;
  }
}

const std::filesystem::path* g_mkdir_on_progress_path = nullptr;

void CreateDirOnReadProgress(int state, int /*percent*/) {
  if (state == 1 && g_mkdir_on_progress_path != nullptr) {
    std::error_code ec;
    std::filesystem::create_directory(*g_mkdir_on_progress_path, ec);
  }
}

// RAII guard for a single GSF file handle (`gsfOpen` / `gsfClose`), ensuring
// slots in the 4-entry `gsfFileTable` are always released on early returns.
class ScopedGsfFile {
 public:
  ScopedGsfFile() = default;
  ~ScopedGsfFile() { Close(); }

  ScopedGsfFile(const ScopedGsfFile&) = delete;
  ScopedGsfFile& operator=(const ScopedGsfFile&) = delete;

  int Open(const char* path, int mode) {
    Close();
    int h = 0;
    const int rc = gsfOpen(path, mode, &h);
    if (rc == GSF_NORMAL) {
      handle_ = h;
    }
    return rc;
  }

  int Close() {
    if (handle_ > 0) {
      const int rc = gsfClose(handle_);
      handle_ = 0;
      return rc;
    }
    return GSF_NORMAL;
  }

  int handle() const { return handle_; }

 private:
  int handle_ = 0;
};

std::filesystem::path IndexPathForGsf(const std::filesystem::path& gsf_path) {
  std::string s = gsf_path.native();
  s[s.size() - 3] = 'n';
  return std::filesystem::path(s);
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  EXPECT_TRUE(in.is_open()) << path;
  if (!in.is_open()) {
    return {};
  }
  const std::streamsize size = in.tellg();
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(static_cast<std::size_t>(size));
  if (size > 0) {
    in.read(reinterpret_cast<char*>(buf.data()), size);
  }
  return buf;
}

void WriteFileBytes(const std::filesystem::path& path,
                    std::span<const uint8_t> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << path;
  if (!bytes.empty()) {
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  ASSERT_TRUE(out.good()) << path;
}

void AppendRawBytes(const std::filesystem::path& path,
                    std::span<const uint8_t> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::app);
  ASSERT_TRUE(out.is_open()) << path;
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(out.good()) << path;
}

template <typename T>
void WriteVal(std::span<uint8_t> dst, std::size_t offset, T val) {
  static_assert(std::is_trivially_copyable_v<T>);
  std::memcpy(dst.data() + offset, &val, sizeof(T));
}

template <typename T>
T ReadVal(std::span<const uint8_t> src, std::size_t offset) {
  static_assert(std::is_trivially_copyable_v<T>);
  T val{};
  std::memcpy(&val, src.data() + offset, sizeof(T));
  return val;
}

// Builds a minimal 48-byte GSF v2 index header (followed optionally by
// `extra_bytes` of zero-initialized payload).
std::vector<uint8_t> MakeIndexHeaderBytes(std::string_view version_16,
                                          long long gsf_file_size, int endian,
                                          int num_record_types,
                                          std::size_t extra_bytes = 0) {
  std::vector<uint8_t> buf(48 + extra_bytes, 0);
  const std::size_t copy_len = std::min<std::size_t>(version_16.size(), 16);
  std::memcpy(buf.data(), version_16.data(), copy_len);
  WriteVal<long long>(buf, 16, gsf_file_size);
  WriteVal<int>(buf, 24, endian);
  WriteVal<int>(buf, 28, num_record_types);
  return buf;
}

void WritePingRecord(int handle, time_t sec, long nsec, int offset) {
  gsfDataID id{};
  std::memset(&id, 0, sizeof(id));
  id.recordID = GSF_RECORD_SWATH_BATHYMETRY_PING;

  gsfRecords rec{};
  std::memset(&rec, 0, sizeof(rec));
  std::array<double, 4> depths = {100.1, 100.2, 100.3, 100.4};
  rec.mb_ping.ping_time.tv_sec = sec;
  rec.mb_ping.ping_time.tv_nsec = nsec;
  rec.mb_ping.latitude = 10.5;
  rec.mb_ping.longitude = -20.5;
  rec.mb_ping.number_beams = static_cast<short>(depths.size());
  rec.mb_ping.center_beam = 2;
  // Use a non-empty sensor subrecord so `(record_size - bytes) != 4` at
  // gsf_dec.c:1891, avoiding uninitialized `records.mb_ping.sensor_id` reads
  // inside gsfCreateIndexFile/gsfAppendIndexFile (`gsf_indx.c:516, 1311`).
  rec.mb_ping.sensor_id = GSF_SWATH_BATHY_SUBRECORD_RESON_8101_SPECIFIC;
  rec.mb_ping.depth = depths.data();
  ASSERT_EQ(GSF_NORMAL,
            gsfLoadScaleFactor(&rec.mb_ping.scaleFactors,
                               GSF_SWATH_BATHY_SUBRECORD_DEPTH_ARRAY,
                               GSF_DISABLE_COMPRESSION | GSF_FIELD_SIZE_TWO,
                               0.01, offset));
  ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
}

void WriteRecordOfType(int handle, unsigned int record_id, int seq) {
  gsfDataID id{};
  std::memset(&id, 0, sizeof(id));
  id.recordID = record_id;
  gsfRecords rec{};
  std::memset(&rec, 0, sizeof(rec));
  const time_t base_sec = 1700000000 + static_cast<time_t>(seq) * 10;
  const long base_nsec = static_cast<long>(seq) * 1000 + 100;

  switch (record_id) {
    case GSF_RECORD_SWATH_BATHY_SUMMARY: {
      // gsf_indx.c indexes summary with start_time.tv_sec and end_time.tv_nsec.
      rec.summary.start_time.tv_sec = base_sec;
      rec.summary.start_time.tv_nsec = 111;
      rec.summary.end_time.tv_sec = base_sec + 50;
      rec.summary.end_time.tv_nsec = base_nsec;
      rec.summary.min_latitude = 10.0;
      rec.summary.max_latitude = 11.0;
      rec.summary.min_longitude = -21.0;
      rec.summary.max_longitude = -20.0;
      rec.summary.min_depth = 50.0;
      rec.summary.max_depth = 150.0;
      ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
      break;
    }
    case GSF_RECORD_SWATH_BATHYMETRY_PING:
      WritePingRecord(handle, base_sec, base_nsec, /*offset=*/0);
      break;
    case GSF_RECORD_SOUND_VELOCITY_PROFILE: {
      std::array<double, 2> svp_depths = {0.0, 50.0};
      std::array<double, 2> svp_speeds = {1500.0, 1495.0};
      rec.svp.observation_time.tv_sec = base_sec - 1;
      rec.svp.observation_time.tv_nsec = 500;
      rec.svp.application_time.tv_sec = base_sec;
      rec.svp.application_time.tv_nsec = base_nsec;
      rec.svp.latitude = 10.5;
      rec.svp.longitude = -20.5;
      rec.svp.number_points = 2;
      rec.svp.depth = svp_depths.data();
      rec.svp.sound_speed = svp_speeds.data();
      ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
      break;
    }
    case GSF_RECORD_PROCESSING_PARAMETERS: {
      char param_buf[] = "ROLL_COMPENSATED=YES";
      rec.process_parameters.param_time.tv_sec = base_sec;
      rec.process_parameters.param_time.tv_nsec = base_nsec;
      rec.process_parameters.number_parameters = 1;
      rec.process_parameters.param[0] = param_buf;
      ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
      break;
    }
    case GSF_RECORD_SENSOR_PARAMETERS: {
      char param_buf[] = "TX_POWER=HIGH";
      rec.sensor_parameters.param_time.tv_sec = base_sec;
      rec.sensor_parameters.param_time.tv_nsec = base_nsec;
      rec.sensor_parameters.number_parameters = 1;
      rec.sensor_parameters.param[0] = param_buf;
      ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
      break;
    }
    case GSF_RECORD_COMMENT: {
      char comment_buf[] = "index test comment";
      rec.comment.comment_time.tv_sec = base_sec;
      rec.comment.comment_time.tv_nsec = base_nsec;
      rec.comment.comment_length = static_cast<int>(std::strlen(comment_buf));
      rec.comment.comment = comment_buf;
      ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
      break;
    }
    case GSF_RECORD_HISTORY: {
      // gsfDecodeHistory requires non-empty host_name and operator_name.
      constexpr std::string_view kHost = "testhost";
      constexpr std::string_view kOperator = "tester";
      std::memcpy(rec.history.host_name, kHost.data(), kHost.size());
      std::memcpy(rec.history.operator_name, kOperator.data(), kOperator.size());
      char cmd_buf[] = "gsf_indx_test";
      char comment_buf[] = "history entry";
      rec.history.history_time.tv_sec = base_sec;
      rec.history.history_time.tv_nsec = base_nsec;
      rec.history.command_line = cmd_buf;
      rec.history.comment = comment_buf;
      ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
      break;
    }
    case GSF_RECORD_NAVIGATION_ERROR:
      rec.nav_error.nav_error_time.tv_sec = base_sec;
      rec.nav_error.nav_error_time.tv_nsec = base_nsec;
      rec.nav_error.record_id = GSF_RECORD_SWATH_BATHYMETRY_PING;
      rec.nav_error.latitude_error = 1.2;
      rec.nav_error.longitude_error = 2.3;
      ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
      break;
    case GSF_RECORD_SINGLE_BEAM_PING:
      rec.sb_ping.ping_time.tv_sec = base_sec;
      rec.sb_ping.ping_time.tv_nsec = base_nsec;
      rec.sb_ping.latitude = 10.5;
      rec.sb_ping.longitude = -20.5;
      rec.sb_ping.heading = 90.0;
      rec.sb_ping.depth = 45.6;
      rec.sb_ping.sensor_id = GSF_SINGLE_BEAM_SUBRECORD_ECHOTRAC_SPECIFIC;
      rec.sb_ping.sensor_data.gsfEchotracSpecific.navigation_error = 1;
      rec.sb_ping.sensor_data.gsfEchotracSpecific.mpp_source = 1;
      rec.sb_ping.sensor_data.gsfEchotracSpecific.tide_source = 1;
      ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
      break;
    case GSF_RECORD_HV_NAVIGATION_ERROR: {
      char pos_type[] = GSF_POS_TYPE_GPSU;
      rec.hv_nav_error.nav_error_time.tv_sec = base_sec;
      rec.hv_nav_error.nav_error_time.tv_nsec = base_nsec;
      rec.hv_nav_error.record_id = GSF_RECORD_SWATH_BATHYMETRY_PING;
      rec.hv_nav_error.horizontal_error = 0.5;
      rec.hv_nav_error.vertical_error = 0.25;
      rec.hv_nav_error.SEP_uncertainty = 0.1;
      rec.hv_nav_error.position_type = pos_type;
      ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
      break;
    }
    case GSF_RECORD_ATTITUDE: {
      std::array<struct timespec, 2> att_times = {
          {{base_sec, base_nsec}, {base_sec, base_nsec + 500000}}};
      std::array<double, 2> pitch = {1.0, 1.5};
      std::array<double, 2> roll = {-0.5, 0.5};
      std::array<double, 2> heave = {0.1, -0.1};
      std::array<double, 2> heading = {180.0, 180.5};
      rec.attitude.num_measurements = 2;
      rec.attitude.attitude_time = att_times.data();
      rec.attitude.pitch = pitch.data();
      rec.attitude.roll = roll.data();
      rec.attitude.heave = heave.data();
      rec.attitude.heading = heading.data();
      ASSERT_GT(gsfWrite(handle, &id, &rec), 0);
      break;
    }
    default:
      FAIL() << "Unexpected record_id: " << record_id;
  }
}

constexpr auto kAllIndexedTypes = std::to_array<unsigned int>({
    GSF_RECORD_SWATH_BATHY_SUMMARY,
    GSF_RECORD_SWATH_BATHYMETRY_PING,
    GSF_RECORD_SOUND_VELOCITY_PROFILE,
    GSF_RECORD_PROCESSING_PARAMETERS,
    GSF_RECORD_SENSOR_PARAMETERS,
    GSF_RECORD_COMMENT,
    GSF_RECORD_HISTORY,
    GSF_RECORD_NAVIGATION_ERROR,
    GSF_RECORD_SINGLE_BEAM_PING,
    GSF_RECORD_HV_NAVIGATION_ERROR,
    GSF_RECORD_ATTITUDE,
});

// Invokes `gsfOpenIndex` directly on a stack `GSF_FILE_TABLE` while closing
// `ft.index_data.fp`, `ft.fp`, and freeing `scale_factor_addr` (avoiding the
// `gsfFileTable` FILE* leak at `gsf.c:506` on index error paths). Callers must
// NOT use this helper for `gsfCreateIndexFile` failure paths occurring after
// `gsf_indx.c:476` (which calls `fclose(ft->index_data.fp)` without nulling the
// pointer, risking double-`fclose` during cleanup).
int CallOpenIndexDirect(const std::filesystem::path& gsf_path, int handle,
                        long long file_size, int initial_swap,
                        bool open_gsf_fp) {
  GSF_FILE_TABLE ft{};
  std::memset(&ft, 0, sizeof(ft));
  ft.file_size = file_size;
  ft.index_data.swap = initial_swap;
  if (open_gsf_fp) {
    ft.fp = std::fopen(gsf_path.c_str(), "rb");
    EXPECT_NE(nullptr, ft.fp);
  }
  absl::Cleanup cleanup = [&ft]() {
    if (ft.index_data.fp != nullptr) {
      gsfCloseIndex(&ft);
    }
    if (ft.fp != nullptr) {
      std::fclose(ft.fp);
      ft.fp = nullptr;
    }
    if (ft.index_data.scale_factor_addr != nullptr) {
      std::free(ft.index_data.scale_factor_addr);
      ft.index_data.scale_factor_addr = nullptr;
    }
  };
  gsfError = GSF_NORMAL;
  return gsfOpenIndex(gsf_path.c_str(), handle, &ft);
}

// Opens and closes a fresh unique header-only `.gsf` file so `gsfOpen`
// (`gsf.c:350`) picks the idle `gsfFileTable` slot for a non-matching filename
// and calls `gsfFree(&gsfFileTable[slot].rec)`. This avoids `gsfSeekRecord`'s
// `memcpy(&ft->rec, &scalesRecord)` (`gsf.c:1387`) leaking prior non-ping
// allocations when reopening the same path in indexed mode or appending.
void FlushFileTableSlot(const TempDir& dir) {
  static std::atomic<int> flush_seq{0};
  const std::filesystem::path flush_path = dir.file(std::format(
      "flush_slot_{}.gsf", flush_seq.fetch_add(1, std::memory_order_relaxed)));
  {
    ScopedGsfFile flush_gsf;
    EXPECT_EQ(GSF_NORMAL, flush_gsf.Open(flush_path.c_str(), GSF_CREATE));
    EXPECT_EQ(GSF_NORMAL, flush_gsf.Close());
  }
  std::error_code ec;
  std::filesystem::remove(flush_path, ec);
}

TEST(GsfIndxTest, SwapLongReversesMultiElementByteOrder) {
  std::array<unsigned int, 4> values = {0x01020304U, 0xAABBCCDDU, 0x00000000U,
                                        0x11223344U};
  SwapLong(values.data(), 0);
  EXPECT_THAT(values,
              ElementsAre(0x01020304U, 0xAABBCCDDU, 0x00000000U, 0x11223344U));

  SwapLong(values.data(), static_cast<int>(values.size()));
  EXPECT_THAT(values,
              ElementsAre(0x04030201U, 0xDDCCBBAAU, 0x00000000U, 0x44332211U));

  SwapLong(values.data(), static_cast<int>(values.size()));
  EXPECT_THAT(values,
              ElementsAre(0x01020304U, 0xAABBCCDDU, 0x00000000U, 0x11223344U));
}

TEST(GsfIndxTest, SwapLongLongZeroCountAvoidsUpstreamStackOob) {
  // Zero-count must leave input untouched without entering the byte loop.
  // TODO(schwehr): Upstream `SwapLongLong` (src/gsf_indx.c:2167) writes
  // `data.bytevalue[8 - j] = data.bytevalue[j]` at `j = 0`, writing 1 byte
  // past `bytevalue[8]` — UB in every build configuration, so `count > 0`
  // must never be called until upstream is fixed.
  std::array<long long, 2> values = {0x1234560000000000LL,
                                     0x0102030405060708LL};
  SwapLongLong(values.data(), 0);
  EXPECT_THAT(values, ElementsAre(0x1234560000000000LL, 0x0102030405060708LL));
}

TEST(GsfIndxTest, CreateIndexAllRecordTypesProgressAndRandomSeek) {
  TempDir dir("create_all_types");
  absl::Cleanup flush_slot = [&dir]() { FlushFileTableSlot(dir); };
  ScopedEnvVar env("GSFTMPDIR", dir.string());
  const std::filesystem::path gsf_path = dir.file("all_types.gsf");

  // 1. Build a GSF file with all 12 record types: 1 summary, 25 pings sharing
  // the initial scale factors, 1 ping with updated scale factors, and 1 of each
  // remaining record type (total 26 pings, 2 scale-factor pings).
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
    ASSERT_NO_FATAL_FAILURE(
        WriteRecordOfType(gsf.handle(), GSF_RECORD_SWATH_BATHY_SUMMARY, 1));
    for (int i = 0; i < 25; ++i) {
      ASSERT_NO_FATAL_FAILURE(
          WritePingRecord(gsf.handle(), 1700000100 + i, 1000 + i, /*offset=*/0));
    }
    ASSERT_NO_FATAL_FAILURE(
        WritePingRecord(gsf.handle(), 1700000200, 9999, /*offset=*/5));
    for (unsigned int rec_id : kAllIndexedTypes) {
      if (rec_id == GSF_RECORD_SWATH_BATHY_SUMMARY ||
          rec_id == GSF_RECORD_SWATH_BATHYMETRY_PING) {
        continue;
      }
      ASSERT_NO_FATAL_FAILURE(
          WriteRecordOfType(gsf.handle(), rec_id, static_cast<int>(rec_id)));
    }
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }

  // 2. Register progress callback and create `.nsf` via GSF_READONLY_INDEX.
  g_progress_tracker = {};
  gsf_register_progress_callback(&RecordProgress);
  absl::Cleanup reset_cb = []() {
    gsf_register_progress_callback(nullptr);
    g_progress_tracker = {};
  };

  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));

    // Verify state 1 (reading GSF) and state 2 (writing index) both fired and
    // progressed through multiple percentages.
    int state1_count = 0;
    int state2_count = 0;
    int max_state1_pct = -1;
    int max_state2_pct = -1;
    for (int i = 0; i < g_progress_tracker.count; ++i) {
      if (g_progress_tracker.states[i] == 1) {
        ++state1_count;
        max_state1_pct =
            std::max(max_state1_pct, g_progress_tracker.percents[i]);
      } else if (g_progress_tracker.states[i] == 2) {
        ++state2_count;
        max_state2_pct =
            std::max(max_state2_pct, g_progress_tracker.percents[i]);
      }
    }
    EXPECT_GT(state1_count, 1);
    EXPECT_GT(state2_count, 1);
    EXPECT_EQ(100, max_state1_pct);
    EXPECT_EQ(100, max_state2_pct);
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  FlushFileTableSlot(dir);

  // 3. Reopen the existing native `.nsf` (gsfFileSize == ft.file_size) and
  // verify record counts, indexed timestamps, and random seeks across scale
  // factor transitions.
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(2, gsfGetNumberRecords(gsf.handle(), 0));
    EXPECT_EQ(26, gsfGetNumberRecords(gsf.handle(),
                                      GSF_RECORD_SWATH_BATHYMETRY_PING));
    for (unsigned int rec_id : kAllIndexedTypes) {
      if (rec_id == GSF_RECORD_SWATH_BATHYMETRY_PING) {
        continue;
      }
      EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(), static_cast<int>(rec_id)))
          << "record_id=" << rec_id;
    }

    time_t sec = 0;
    long nsec = 0;
    EXPECT_EQ(1, gsfIndexTime(gsf.handle(), GSF_RECORD_SWATH_BATHY_SUMMARY, 1,
                              &sec, &nsec));
    EXPECT_EQ(1700000010, sec);
    EXPECT_EQ(1100, nsec);

    // Random seek: ping 2 (first scale factors) -> ping 26 (second scale
    // factors) -> back to ping 2 -> attitude -> single-beam.
    gsfDataID id{};
    std::memset(&id, 0, sizeof(id));
    gsfRecords rec{};
    std::memset(&rec, 0, sizeof(rec));
    id.record_number = 2;
    ASSERT_GT(gsfRead(gsf.handle(), GSF_RECORD_SWATH_BATHYMETRY_PING, &id, &rec,
                      nullptr, 0),
              0);
    EXPECT_EQ(1700000101, rec.mb_ping.ping_time.tv_sec);
    ASSERT_NE(nullptr, rec.mb_ping.depth);
    EXPECT_NEAR(100.1, rec.mb_ping.depth[0], 0.01);

    id.record_number = 26;
    ASSERT_GT(gsfRead(gsf.handle(), GSF_RECORD_SWATH_BATHYMETRY_PING, &id, &rec,
                      nullptr, 0),
              0);
    EXPECT_EQ(1700000200, rec.mb_ping.ping_time.tv_sec);
    ASSERT_NE(nullptr, rec.mb_ping.depth);
    EXPECT_NEAR(100.1, rec.mb_ping.depth[0], 0.01);

    id.record_number = 2;
    ASSERT_GT(gsfRead(gsf.handle(), GSF_RECORD_SWATH_BATHYMETRY_PING, &id, &rec,
                      nullptr, 0),
              0);
    EXPECT_EQ(1700000101, rec.mb_ping.ping_time.tv_sec);

    id.record_number = 1;
    ASSERT_GT(gsfRead(gsf.handle(), GSF_RECORD_ATTITUDE, &id, &rec, nullptr, 0),
              0);
    EXPECT_EQ(2, rec.attitude.num_measurements);

    id.record_number = 1;
    ASSERT_GT(gsfRead(gsf.handle(), GSF_RECORD_SINGLE_BEAM_PING, &id, &rec,
                      nullptr, 0),
              0);
    EXPECT_NEAR(45.6, rec.sb_ping.depth, 0.01);

    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
}

TEST(GsfIndxTest, UpdateIndexModeWithoutScaleFactorPings) {
  TempDir dir("update_index_no_pings");
  absl::Cleanup flush_slot = [&dir]() { FlushFileTableSlot(dir); };
  ScopedEnvVar env("GSFTMPDIR", dir.string());
  const std::filesystem::path gsf_path = dir.file("comments_only.gsf");

  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
    ASSERT_NO_FATAL_FAILURE(
        WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 1));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }

  // Create via GSF_UPDATE_INDEX when no scale-factor pings exist
  // (record_type[0] == -1), then reopen via GSF_READONLY_INDEX.
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_UPDATE_INDEX));
    EXPECT_EQ(0, gsfGetNumberRecords(gsf.handle(), 0));
    EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(), GSF_RECORD_COMMENT));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(0, gsfGetNumberRecords(gsf.handle(), 0));
    EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(), GSF_RECORD_COMMENT));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
}

TEST(GsfIndxTest, CreateIndexToleratesPartialEofAndRejectsCorruptRecord) {
  TempDir dir("partial_and_corrupt");
  absl::Cleanup flush_slot = [&dir]() { FlushFileTableSlot(dir); };
  ScopedEnvVar env("GSFTMPDIR", dir.string());

  // 1. Trailing partial record at EOF is ignored by gsfCreateIndexFile.
  const std::filesystem::path partial_gsf = dir.file("partial.gsf");
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(partial_gsf.c_str(), GSF_CREATE));
    ASSERT_NO_FATAL_FAILURE(
        WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 1));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  const std::array<uint8_t, 4> kPartialHeaderBytes = {0x00, 0x00, 0x00, 0x10};
  AppendRawBytes(partial_gsf, kPartialHeaderBytes);
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(partial_gsf.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(), GSF_RECORD_COMMENT));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }

  // 2. A corrupt record immediately after the header triggers a non-EOF
  // gsfRead error (`GSF_RECORD_SIZE_ERROR`) in gsfCreateIndexFile. Because no
  // indexable record precedes it, no temp `.ndx` files are opened or leaked.
  const std::filesystem::path corrupt_gsf = dir.file("corrupt_body.gsf");
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(corrupt_gsf.c_str(), GSF_CREATE));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  // 8-byte record header with invalid dataSize=4 (<= 8 -> GSF_RECORD_SIZE_ERROR).
  const std::array<uint8_t, 8> kBadSizeHeader = {0x00, 0x00, 0x00, 0x04,
                                                 0x00, 0x00, 0x00, 0x06};
  AppendRawBytes(corrupt_gsf, kBadSizeHeader);
  {
    ScopedGsfFile gsf;
    EXPECT_EQ(-1, gsf.Open(corrupt_gsf.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(GSF_INDEX_FILE_OPEN_ERROR, gsfIntError());
  }
}

TEST(GsfIndxTest, OpenIndexRejectsNdxFilenameCollision) {
  TempDir dir("ndx_collision");
  ScopedEnvVar env("GSFTMPDIR", dir.string());
  const std::filesystem::path ndx_gsf = dir.file("collision.ndx");

  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(ndx_gsf.c_str(), GSF_CREATE));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }

  // `ndx_file[len - 3] = 'n'` keeps `.ndx` unchanged; existing file collides.
  {
    ScopedGsfFile gsf;
    EXPECT_EQ(-1, gsf.Open(ndx_gsf.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(GSF_INDEX_FILE_OPEN_ERROR, gsfIntError());
  }
  EXPECT_EQ(-1, CallOpenIndexDirect(ndx_gsf, /*handle=*/1, /*file_size=*/0,
                                    /*initial_swap=*/0, /*open_gsf_fp=*/false));
  EXPECT_EQ(GSF_INDEX_FILE_OPEN_ERROR, gsfIntError());
}

TEST(GsfIndxTest, OpenIndexRecreatesEmptyOrNonGsfPrefixedIndexFiles) {
  TempDir dir("recreate_bad_prefix");
  absl::Cleanup flush_slot = [&dir]() { FlushFileTableSlot(dir); };
  ScopedEnvVar env("GSFTMPDIR", dir.string());
  const std::filesystem::path gsf_path = dir.file("sample.gsf");
  const std::filesystem::path nsf_path = IndexPathForGsf(gsf_path);

  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
    ASSERT_NO_FATAL_FAILURE(
        WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 1));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }

  // Case 1: Empty `.nsf` (`version[0] == 0`) triggers index recreation.
  WriteFileBytes(nsf_path, std::span<const uint8_t>{});
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(), GSF_RECORD_COMMENT));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }

  // Case 2: Non-"INDEX-GSF-" prefix triggers index recreation.
  const std::array<uint8_t, 16> kWrongPrefix = {'N', 'O', 'T', '-', 'I', 'N',
                                                'D', 'E', 'X', '-', '0', '0',
                                                '0', '0', '0', '0'};
  WriteFileBytes(nsf_path, kWrongPrefix);
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(), GSF_RECORD_COMMENT));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  std::vector<uint8_t> rebuilt = ReadFileBytes(nsf_path);
  ASSERT_GE(rebuilt.size(), static_cast<std::size_t>(GSF_INDEX_VERSION_SIZE));
  EXPECT_EQ(GSF_INDEX_VERSION,
            std::string_view(reinterpret_cast<const char*>(rebuilt.data()),
                             GSF_INDEX_VERSION_SIZE));

  // Case 3: Read-only `.nsf` with bad prefix fails recreation at line 243.
  if (geteuid() != 0) {
    WriteFileBytes(nsf_path, kWrongPrefix);
    ASSERT_EQ(0, chmod(nsf_path.c_str(), 0444));
    absl::Cleanup restore_perm = [&nsf_path]() { chmod(nsf_path.c_str(), 0644); };
    ScopedGsfFile gsf;
    EXPECT_EQ(-1, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(GSF_INDEX_FILE_OPEN_ERROR, gsfIntError());
  }
}

TEST(GsfIndxTest, OpenIndexRejectsMalformedVersionString) {
  TempDir dir("malformed_version");
  ScopedEnvVar env("GSFTMPDIR", dir.string());
  const std::filesystem::path gsf_path = dir.file("sample.gsf");
  const std::filesystem::path nsf_path = IndexPathForGsf(gsf_path);

  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  long long file_size = 0;
  ASSERT_EQ(GSF_NORMAL, gsfStat(gsf_path.c_str(), &file_size));

  const std::vector<uint8_t> bad_ver =
      MakeIndexHeaderBytes("INDEX-GSF-vBAD!!", file_size, 0x00010203, 1);
  WriteFileBytes(nsf_path, bad_ver);

  EXPECT_EQ(-1, CallOpenIndexDirect(gsf_path, /*handle=*/1, file_size,
                                    /*initial_swap=*/0, /*open_gsf_fp=*/false));
  EXPECT_EQ(GSF_UNRECOGNIZED_FILE, gsfIntError());
}

TEST(GsfIndxTest, OpenIndexRecreatesLegacyV01Index) {
  TempDir dir("legacy_v01");
  absl::Cleanup flush_slot = [&dir]() { FlushFileTableSlot(dir); };
  ScopedEnvVar env("GSFTMPDIR", dir.string());
  const std::filesystem::path gsf_path = dir.file("legacy.gsf");
  const std::filesystem::path nsf_path = IndexPathForGsf(gsf_path);

  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
    ASSERT_NO_FATAL_FAILURE(
        WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 1));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }

  const std::vector<uint8_t> legacy_hdr =
      MakeIndexHeaderBytes("INDEX-GSF-v01.01", 0, 0x00010203, 1);
  WriteFileBytes(nsf_path, legacy_hdr);

  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(), GSF_RECORD_COMMENT));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  std::vector<uint8_t> upgraded = ReadFileBytes(nsf_path);
  ASSERT_GE(upgraded.size(), static_cast<std::size_t>(GSF_INDEX_VERSION_SIZE));
  EXPECT_EQ(GSF_INDEX_VERSION,
            std::string_view(reinterpret_cast<const char*>(upgraded.data()),
                             GSF_INDEX_VERSION_SIZE));

  // Read-only v01.01 `.nsf` fails recreation at line 275.
  if (geteuid() != 0) {
    WriteFileBytes(nsf_path, legacy_hdr);
    ASSERT_EQ(0, chmod(nsf_path.c_str(), 0444));
    absl::Cleanup restore_perm = [&nsf_path]() { chmod(nsf_path.c_str(), 0644); };
    ScopedGsfFile gsf;
    EXPECT_EQ(-1, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(GSF_INDEX_FILE_OPEN_ERROR, gsfIntError());
  }
}

TEST(GsfIndxTest, OpenIndexRejectsBadEndianAndInvalidRecordTypeCounts) {
  TempDir dir("corrupt_header_fields");
  ScopedEnvVar env("GSFTMPDIR", dir.string());
  const std::filesystem::path gsf_path = dir.file("sample.gsf");
  const std::filesystem::path nsf_path = IndexPathForGsf(gsf_path);

  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  long long file_size = 0;
  ASSERT_EQ(GSF_NORMAL, gsfStat(gsf_path.c_str(), &file_size));

  struct CorruptHeaderCase {
    int endian;
    int num_record_types;
  };
  constexpr CorruptHeaderCase kCases[] = {
      {.endian = 0x11223344, .num_record_types = 1},
      {.endian = 0x00010203, .num_record_types = 0},
      {.endian = 0x00010203, .num_record_types = NUM_REC_TYPES + 1},
  };

  for (const auto& tc : kCases) {
    const std::vector<uint8_t> hdr = MakeIndexHeaderBytes(
        GSF_INDEX_VERSION, file_size, tc.endian, tc.num_record_types);
    WriteFileBytes(nsf_path, hdr);
    EXPECT_EQ(-1,
              CallOpenIndexDirect(gsf_path, /*handle=*/1, file_size,
                                  /*initial_swap=*/0, /*open_gsf_fp=*/false));
    EXPECT_EQ(GSF_CORRUPT_INDEX_FILE_ERROR, gsfIntError());
  }
}

TEST(GsfIndxTest, OpenIndexRecreatesWhenGsfFileShrunk) {
#if defined(ADDRESS_SANITIZER) || defined(ABSL_HAVE_ADDRESS_SANITIZER) || \
    defined(LEAK_SANITIZER) || defined(ABSL_HAVE_LEAK_SANITIZER)
  GTEST_SKIP()
      << "Skipped under ASan/LSan: upstream gsf_indx.c:319 calls remove() "
      << "without closing ft->index_data.fp before gsfCreateIndexFile.";
#else
  TempDir dir("shrunk_gsf");
  ScopedEnvVar env("GSFTMPDIR", dir.string());
  const std::filesystem::path gsf_path = dir.file("shrunk.gsf");
  const std::filesystem::path nsf_path = IndexPathForGsf(gsf_path);

  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
    ASSERT_NO_FATAL_FAILURE(
        WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 1));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  long long file_size = 0;
  ASSERT_EQ(GSF_NORMAL, gsfStat(gsf_path.c_str(), &file_size));

  // Write an `.nsf` claiming a larger `gsfFileSize` than the current GSF file.
  WriteFileBytes(nsf_path, MakeIndexHeaderBytes(GSF_INDEX_VERSION,
                                                file_size + 512, 0x00010203, 1));
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(), GSF_RECORD_COMMENT));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  const std::vector<uint8_t> rebuilt = ReadFileBytes(nsf_path);
  ASSERT_GE(rebuilt.size(), 24u);
  EXPECT_EQ(file_size, ReadVal<long long>(rebuilt, 16));

  // Recreate failure branch (`gsf_indx.c:321`) when dir prevents unlinking and
  // file is read-only.
  if (geteuid() != 0) {
    WriteFileBytes(
        nsf_path,
        MakeIndexHeaderBytes(GSF_INDEX_VERSION, file_size + 512, 0x00010203, 1));
    ASSERT_EQ(0, chmod(nsf_path.c_str(), 0444));
    ASSERT_EQ(0, chmod(dir.c_str(), 0555));
    absl::Cleanup restore_perms = [&dir, &nsf_path]() {
      chmod(dir.c_str(), 0755);
      chmod(nsf_path.c_str(), 0644);
    };
    ScopedGsfFile gsf;
    EXPECT_EQ(-1, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(GSF_INDEX_FILE_OPEN_ERROR, gsfIntError());
  }
#endif
}

TEST(GsfIndxTest, OpenAndAppendSwappedEndianHeaderValidatesRecordTypeCount) {
  TempDir dir("swapped_header_validation");
  ScopedEnvVar env("GSFTMPDIR", dir.string());
  const std::filesystem::path gsf_path = dir.file("swapped_hdr.gsf");
  const std::filesystem::path nsf_path = IndexPathForGsf(gsf_path);

  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
    ASSERT_NO_FATAL_FAILURE(
        WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 1));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  long long file_size = 0;
  ASSERT_EQ(GSF_NORMAL, gsfStat(gsf_path.c_str(), &file_size));

  // Verify big-endian (`0x03020100`) record-type count decoding via `SwapLong`
  // on open (`gsf_indx.c:290-293, 332-335`) and append (`gsf_indx.c:1098-1101`)
  // without invoking `SwapLongLong` (which has a 1-byte stack OOB store at
  // `gsf_indx.c:2167` when `count > 0`).
  for (const int bad_wire_count : {0, 0x0E000000}) {
    WriteFileBytes(nsf_path, MakeIndexHeaderBytes(GSF_INDEX_VERSION, file_size,
                                                  0x03020100, bad_wire_count));
    EXPECT_EQ(-1, CallOpenIndexDirect(gsf_path, /*handle=*/1, file_size,
                                      /*initial_swap=*/0,
                                      /*open_gsf_fp=*/false));
    EXPECT_EQ(GSF_CORRUPT_INDEX_FILE_ERROR, gsfIntError());

    WriteFileBytes(nsf_path, MakeIndexHeaderBytes(GSF_INDEX_VERSION,
                                                  /*gsf_file_size=*/12,
                                                  0x03020100, bad_wire_count));
    EXPECT_EQ(-1, CallOpenIndexDirect(gsf_path, /*handle=*/1, file_size,
                                      /*initial_swap=*/0,
                                      /*open_gsf_fp=*/false));
    EXPECT_EQ(GSF_CORRUPT_INDEX_FILE_ERROR, gsfIntError());
  }
}

TEST(GsfIndxTest, CreateIndexFailsInReadOnlyDirectory) {
  if (geteuid() == 0) {
    GTEST_SKIP() << "Root ignores 0555 directory permissions.";
  }
  TempDir dir("readonly_dir");
  absl::Cleanup flush_slot = [&dir]() { FlushFileTableSlot(dir); };
  ScopedEnvVar env("GSFTMPDIR", dir.string());
  const std::filesystem::path gsf_path = dir.file("ro.gsf");

  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  ASSERT_EQ(0, chmod(dir.c_str(), 0555));
  absl::Cleanup restore_dir = [&dir]() { chmod(dir.c_str(), 0755); };

  ScopedGsfFile gsf;
  EXPECT_EQ(-1, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
  EXPECT_EQ(GSF_INDEX_FILE_OPEN_ERROR, gsfIntError());
}

TEST(GsfIndxTest, CreateIndexFailsWhenTempDirIsReadOnlyForEachRecordType) {
  if (geteuid() == 0) {
    GTEST_SKIP() << "Root ignores 0555 directory permissions.";
  }
  TempDir ro_tmp("ro_gsftmpdir");
  ASSERT_EQ(0, chmod(ro_tmp.c_str(), 0555));
  absl::Cleanup restore_tmp = [&ro_tmp]() { chmod(ro_tmp.c_str(), 0755); };

  TempDir gsf_dir("ro_gsftmpdir_files");
  absl::Cleanup flush_slot = [&gsf_dir]() { FlushFileTableSlot(gsf_dir); };
  ScopedEnvVar env("GSFTMPDIR", ro_tmp.string());

  // Each file contains a single indexable record right after HEADER so
  // `open_temp_file` fails on the first switch branch without leaking any
  // previously opened temp FILE* descriptors.
  for (unsigned int rec_id : kAllIndexedTypes) {
    const std::filesystem::path gsf_path =
        gsf_dir.file(std::format("type_{}.gsf", rec_id));
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
      ASSERT_NO_FATAL_FAILURE(WriteRecordOfType(gsf.handle(), rec_id, 1));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    ScopedGsfFile gsf;
    EXPECT_EQ(-1, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX))
        << "record_id=" << rec_id;
    EXPECT_EQ(GSF_INDEX_FILE_OPEN_ERROR, gsfIntError());
  }
}

TEST(GsfIndxTest, AppendIndexAddsAllRecordTypesProgressAndReopens) {
  TempDir dir("append_all_types");
  absl::Cleanup flush_slot = [&dir]() { FlushFileTableSlot(dir); };
  ScopedEnvVar env("GSFTMPDIR", dir.string());
  const std::filesystem::path gsf_path = dir.file("append_all.gsf");

  // 1. Seed the GSF file with all 11 indexed record types first so every
  // `record_type[i]` entry exists in the initial `.nsf` (avoiding upstream
  // gsf_indx.c:1786 writing -1 into the TOC before line 1826 populates it).
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
    for (unsigned int rec_id : kAllIndexedTypes) {
      ASSERT_NO_FATAL_FAILURE(
          WriteRecordOfType(gsf.handle(), rec_id, static_cast<int>(rec_id)));
    }
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  FlushFileTableSlot(dir);

  // 2. Append 111 pings (since FlushFileTableSlot cleared cached scale factors,
  // ping 0 triggers `realloc` on `scale_factor_addr` at gsf_indx.c:1399 once
  // without reading uninitialized `realloc` slots on a second scale-factor
  // decode at gsf_dec.c:949) plus every record type (including SINGLE_BEAM_PING).
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_APPEND));
    for (unsigned int rec_id : kAllIndexedTypes) {
      if (rec_id == GSF_RECORD_SWATH_BATHYMETRY_PING) {
        continue;
      }
      ASSERT_NO_FATAL_FAILURE(WriteRecordOfType(
          gsf.handle(), rec_id, 100 + static_cast<int>(rec_id)));
    }
    for (int i = 0; i < 110; ++i) {
      ASSERT_NO_FATAL_FAILURE(
          WritePingRecord(gsf.handle(), 1700001000 + i, 5000 + i, /*offset=*/0));
    }
    ASSERT_NO_FATAL_FAILURE(
        WritePingRecord(gsf.handle(), 1700002000, 7777, /*offset=*/0));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }

  // 3. Reopen indexed to trigger gsfAppendIndexFile and verify state 3 progress
  // callbacks advance across multiple percentages.
  g_progress_tracker = {};
  gsf_register_progress_callback(&RecordProgress);
  absl::Cleanup reset_cb = []() {
    gsf_register_progress_callback(nullptr);
    g_progress_tracker = {};
  };

  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    int state3_count = 0;
    int max_state3_pct = -1;
    for (int i = 0; i < g_progress_tracker.count; ++i) {
      if (g_progress_tracker.states[i] == 3) {
        ++state3_count;
        max_state3_pct =
            std::max(max_state3_pct, g_progress_tracker.percents[i]);
      }
    }
    EXPECT_GT(state3_count, 5);
    EXPECT_EQ(100, max_state3_pct);
    // Single-beam ping is omitted from gsfAppendIndexFile's switch (`default:`).
    // Flushing the file table slot before GSF_APPEND resets cached scaleFactors,
    // so ping 0 of the appended batch adds 1 scale-factor index record (total
    // type-0 records == 2).
    EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(), GSF_RECORD_SINGLE_BEAM_PING));
    EXPECT_EQ(112, gsfGetNumberRecords(gsf.handle(),
                                       GSF_RECORD_SWATH_BATHYMETRY_PING));
    EXPECT_EQ(2, gsfGetNumberRecords(gsf.handle(), 0));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  FlushFileTableSlot(dir);

  // 4. Reopen again (reads updated `.nsf` cleanly via `gsfOpenIndex` so
  // `scale_factor_addr` is reloaded from disk) and verify counts and seeks.
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(2, gsfGetNumberRecords(gsf.handle(), 0));
    EXPECT_EQ(112, gsfGetNumberRecords(gsf.handle(),
                                       GSF_RECORD_SWATH_BATHYMETRY_PING));
    EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(), GSF_RECORD_SINGLE_BEAM_PING));
    for (unsigned int rec_id : kAllIndexedTypes) {
      if (rec_id == GSF_RECORD_SWATH_BATHYMETRY_PING ||
          rec_id == GSF_RECORD_SINGLE_BEAM_PING) {
        continue;
      }
      EXPECT_EQ(2, gsfGetNumberRecords(gsf.handle(), static_cast<int>(rec_id)))
          << "record_id=" << rec_id;
    }

    gsfDataID id{};
    std::memset(&id, 0, sizeof(id));
    gsfRecords rec{};
    std::memset(&rec, 0, sizeof(rec));
    id.record_number = -1;
    ASSERT_GT(gsfRead(gsf.handle(), GSF_RECORD_SWATH_BATHYMETRY_PING, &id, &rec,
                      nullptr, 0),
              0);
    EXPECT_EQ(1700002000, rec.mb_ping.ping_time.tv_sec);

    id.record_number = 2;
    ASSERT_GT(gsfRead(gsf.handle(), GSF_RECORD_ATTITUDE, &id, &rec, nullptr, 0),
              0);
    EXPECT_EQ(2, rec.attitude.num_measurements);

    id.record_number = 2;
    ASSERT_GT(gsfRead(gsf.handle(), GSF_RECORD_HV_NAVIGATION_ERROR, &id, &rec,
                      nullptr, 0),
              0);
    ASSERT_NE(nullptr, rec.hv_nav_error.position_type);
    EXPECT_STREQ(GSF_POS_TYPE_GPSU, rec.hv_nav_error.position_type);
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
}

TEST(GsfIndxTest, AppendIndexCoversNewRecordTypesMissingFromInitialIndex) {
  TempDir dir("append_new_types");
  absl::Cleanup flush_slot = [&dir]() { FlushFileTableSlot(dir); };
  ScopedEnvVar env("GSFTMPDIR", dir.string());
  const std::filesystem::path gsf_path = dir.file("grow_types.gsf");

  // Initial file has only COMMENT (no pings, no other types).
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
    ASSERT_NO_FATAL_FAILURE(
        WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 1));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  FlushFileTableSlot(dir);

  // Append all other record types (plus ping with scale factors so `temp[0]`
  // and every `temp[id] == NULL` branch in gsfAppendIndexFile run).
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_APPEND));
    for (unsigned int rec_id : kAllIndexedTypes) {
      if (rec_id == GSF_RECORD_COMMENT) {
        continue;
      }
      ASSERT_NO_FATAL_FAILURE(
          WriteRecordOfType(gsf.handle(), rec_id, 50 + static_cast<int>(rec_id)));
    }
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }

  // Note: the `.nsf` written by `gsfAppendIndexFile` contains TOC `record_type`
  // == -1 for newly appended record types (`gsf_indx.c:1786` runs before
  // `gsf_indx.c:1826`), so the `.nsf` must not be reopened (`gsf_indx.c:367`
  // would write to `record_type[-1]`). Only inspect in-memory counts and close.
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(), 0));
    EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(), GSF_RECORD_COMMENT));
    for (unsigned int rec_id : kAllIndexedTypes) {
      if (rec_id == GSF_RECORD_COMMENT ||
          rec_id == GSF_RECORD_SINGLE_BEAM_PING) {
        continue;
      }
      EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(), static_cast<int>(rec_id)))
          << "record_id=" << rec_id;
    }
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  FlushFileTableSlot(dir);

  // Also cover newly appended `GSF_RECORD_COMMENT` (`gsf_indx.c:1523-1530`);
  // again do not reopen the resulting `.nsf` because its TOC stores -1.
  const std::filesystem::path svp_cmt_path = dir.file("svp_then_comment.gsf");
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(svp_cmt_path.c_str(), GSF_CREATE));
    ASSERT_NO_FATAL_FAILURE(
        WriteRecordOfType(gsf.handle(), GSF_RECORD_SOUND_VELOCITY_PROFILE, 1));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(svp_cmt_path.c_str(), GSF_READONLY_INDEX));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  FlushFileTableSlot(dir);
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(svp_cmt_path.c_str(), GSF_APPEND));
    ASSERT_NO_FATAL_FAILURE(
        WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 2));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(svp_cmt_path.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(),
                                     GSF_RECORD_SOUND_VELOCITY_PROFILE));
    EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(), GSF_RECORD_COMMENT));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
}

TEST(GsfIndxTest, AppendIndexWhenLastIndexedRecordIsPingAndPartialEof) {
  TempDir dir("append_last_ping");
  absl::Cleanup flush_slot = [&dir]() { FlushFileTableSlot(dir); };
  ScopedEnvVar env("GSFTMPDIR", dir.string());
  const std::filesystem::path gsf_path = dir.file("last_ping.gsf");

  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
    // Write 2 initial pings with identical scale factors so ping 2's `.nsf`
    // address is strictly greater than record_type 0's (gsf_indx.c:1179 uses
    // strict `>` when tracking `last_record_type`), keeping `last_record_type`
    // equal to GSF_RECORD_SWATH_BATHYMETRY_PING on reopen.
    ASSERT_NO_FATAL_FAILURE(
        WritePingRecord(gsf.handle(), 1700000100, 100, /*offset=*/0));
    ASSERT_NO_FATAL_FAILURE(
        WritePingRecord(gsf.handle(), 1700000101, 200, /*offset=*/0));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }

  {
    ScopedGsfFile gsf;
    ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_APPEND));
    ASSERT_NO_FATAL_FAILURE(
        WritePingRecord(gsf.handle(), 1700000102, 300, /*offset=*/0));
    ASSERT_EQ(GSF_NORMAL, gsf.Close());
  }
  const std::array<uint8_t, 4> kTrailingBytes = {0x00, 0x00, 0x00, 0x20};
  AppendRawBytes(gsf_path, kTrailingBytes);

  ScopedGsfFile gsf;
  ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
  EXPECT_EQ(3, gsfGetNumberRecords(gsf.handle(),
                                   GSF_RECORD_SWATH_BATHYMETRY_PING));
  ASSERT_EQ(GSF_NORMAL, gsf.Close());
}

TEST(GsfIndxTest, AppendIndexErrorPathsAndCorruptStates) {
  TempDir dir("append_errors");
  absl::Cleanup flush_slot = [&dir]() { FlushFileTableSlot(dir); };
  ScopedEnvVar env("GSFTMPDIR", dir.string());

  // 1. Corrupt `number_record_types` (`0` and `NUM_REC_TYPES + 1`) on append.
  {
    const std::filesystem::path gsf_path = dir.file("bad_types.gsf");
    const std::filesystem::path nsf_path = IndexPathForGsf(gsf_path);
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
      ASSERT_NO_FATAL_FAILURE(
          WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 1));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    long long file_size = 0;
    ASSERT_EQ(GSF_NORMAL, gsfStat(gsf_path.c_str(), &file_size));

    for (const int bad_count : {0, NUM_REC_TYPES + 1}) {
      WriteFileBytes(nsf_path, MakeIndexHeaderBytes(GSF_INDEX_VERSION, 12,
                                                    0x00010203, bad_count));
      EXPECT_EQ(-1, CallOpenIndexDirect(gsf_path, /*handle=*/1, file_size,
                                        /*initial_swap=*/0,
                                        /*open_gsf_fp=*/false));
      EXPECT_EQ(GSF_CORRUPT_INDEX_FILE_ERROR, gsfIntError());
    }
  }

  // 2. Read-only `GSFTMPDIR` fails `open_temp_file` inside `gsfAppendIndexFile`.
  if (geteuid() != 0) {
    const std::filesystem::path gsf_path = dir.file("ro_tmp_append.gsf");
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
      ASSERT_NO_FATAL_FAILURE(
          WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 1));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_APPEND));
      ASSERT_NO_FATAL_FAILURE(
          WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 2));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    long long file_size = 0;
    ASSERT_EQ(GSF_NORMAL, gsfStat(gsf_path.c_str(), &file_size));

    TempDir ro_tmp("ro_tmp_for_append");
    ASSERT_EQ(0, chmod(ro_tmp.c_str(), 0555));
    absl::Cleanup restore_ro = [&ro_tmp]() { chmod(ro_tmp.c_str(), 0755); };
    ScopedEnvVar ro_env("GSFTMPDIR", ro_tmp.string());

    EXPECT_EQ(-1, CallOpenIndexDirect(gsf_path, /*handle=*/1, file_size,
                                      /*initial_swap=*/0,
                                      /*open_gsf_fp=*/false));
    EXPECT_EQ(GSF_OPEN_TEMP_FILE_FAILED, gsfIntError());
  }

  // 3. Read-only `.nsf` fails reopen (`wb+`) at end of `gsfAppendIndexFile`.
  if (geteuid() != 0) {
    const std::filesystem::path gsf_path = dir.file("ro_nsf_append.gsf");
    const std::filesystem::path nsf_path = IndexPathForGsf(gsf_path);
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
      ASSERT_NO_FATAL_FAILURE(
          WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 1));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_APPEND));
      ASSERT_NO_FATAL_FAILURE(
          WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 2));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    ASSERT_EQ(0, chmod(nsf_path.c_str(), 0444));
    absl::Cleanup restore_nsf = [&nsf_path]() { chmod(nsf_path.c_str(), 0644); };

    ScopedGsfFile gsf;
    EXPECT_EQ(-1, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
    EXPECT_EQ(GSF_INDEX_FILE_OPEN_ERROR, gsfIntError());
  }

  // 4. `gsfRead` failures inside `gsfAppendIndexFile` (last ping seek, last
  // non-ping seek, and appended corrupt record).
  {
    const std::array<uint8_t, 8> kBadSizeRec = {0x00, 0x00, 0x00, 0x04,
                                                0x00, 0x00, 0x00, 0x06};

    // 4a. Fails loading last ping (`gsf_indx.c:1227`).
    const std::filesystem::path ping_fail_gsf = dir.file("append_ping_fail.gsf");
    const std::filesystem::path ping_fail_nsf = IndexPathForGsf(ping_fail_gsf);
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(ping_fail_gsf.c_str(), GSF_CREATE));
      ASSERT_NO_FATAL_FAILURE(
          WritePingRecord(gsf.handle(), 1700000100, 100, /*offset=*/0));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL,
                gsf.Open(ping_fail_gsf.c_str(), GSF_READONLY_INDEX));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    const std::vector<uint8_t> saved_ping_nsf = ReadFileBytes(ping_fail_nsf);
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(ping_fail_gsf.c_str(), GSF_CREATE));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    for (int i = 0; i < 32; ++i) {
      AppendRawBytes(ping_fail_gsf, kBadSizeRec);
    }
    WriteFileBytes(ping_fail_nsf, saved_ping_nsf);
    long long file_size = 0;
    ASSERT_EQ(GSF_NORMAL, gsfStat(ping_fail_gsf.c_str(), &file_size));
    ASSERT_GE(saved_ping_nsf.size(), 24u);
    ASSERT_LT(ReadVal<long long>(saved_ping_nsf, 16), file_size);
    {
      ScopedGsfFile seq;
      ASSERT_EQ(GSF_NORMAL, seq.Open(ping_fail_gsf.c_str(), GSF_READONLY));
      EXPECT_EQ(-1, CallOpenIndexDirect(ping_fail_gsf, seq.handle(), file_size,
                                        /*initial_swap=*/0,
                                        /*open_gsf_fp=*/true));
      EXPECT_EQ(GSF_RECORD_SIZE_ERROR, gsfIntError());
      ASSERT_EQ(GSF_NORMAL, seq.Close());
    }

    // 4b. Fails loading last non-ping record (`gsf_indx.c:1255`).
    const std::filesystem::path cmt_fail_gsf = dir.file("append_cmt_fail.gsf");
    const std::filesystem::path cmt_fail_nsf = IndexPathForGsf(cmt_fail_gsf);
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(cmt_fail_gsf.c_str(), GSF_CREATE));
      ASSERT_NO_FATAL_FAILURE(
          WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 1));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(cmt_fail_gsf.c_str(), GSF_READONLY_INDEX));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    const std::vector<uint8_t> saved_cmt_nsf = ReadFileBytes(cmt_fail_nsf);
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(cmt_fail_gsf.c_str(), GSF_CREATE));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    for (int i = 0; i < 16; ++i) {
      AppendRawBytes(cmt_fail_gsf, kBadSizeRec);
    }
    WriteFileBytes(cmt_fail_nsf, saved_cmt_nsf);
    ASSERT_EQ(GSF_NORMAL, gsfStat(cmt_fail_gsf.c_str(), &file_size));
    ASSERT_GE(saved_cmt_nsf.size(), 24u);
    ASSERT_LT(ReadVal<long long>(saved_cmt_nsf, 16), file_size);
    {
      ScopedGsfFile seq;
      ASSERT_EQ(GSF_NORMAL, seq.Open(cmt_fail_gsf.c_str(), GSF_READONLY));
      EXPECT_EQ(-1, CallOpenIndexDirect(cmt_fail_gsf, seq.handle(), file_size,
                                        /*initial_swap=*/0,
                                        /*open_gsf_fp=*/true));
      EXPECT_EQ(GSF_RECORD_SIZE_ERROR, gsfIntError());
      ASSERT_EQ(GSF_NORMAL, seq.Close());
    }

    // 4c. Fails reading corrupt appended record (`gsf_indx.c:1692`).
    const std::filesystem::path body_fail_gsf =
        dir.file("append_body_fail.gsf");
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(body_fail_gsf.c_str(), GSF_CREATE));
      // Write 2 pings so Ping 2's `.nsf` address beats record_type 0's strict
      // `>` tie-break at line 1179, ensuring `last_record_type == ping`.
      ASSERT_NO_FATAL_FAILURE(
          WritePingRecord(gsf.handle(), 1700000100, 100, /*offset=*/0));
      ASSERT_NO_FATAL_FAILURE(
          WritePingRecord(gsf.handle(), 1700000101, 200, /*offset=*/0));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL,
                gsf.Open(body_fail_gsf.c_str(), GSF_READONLY_INDEX));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    AppendRawBytes(body_fail_gsf, kBadSizeRec);
    ASSERT_EQ(GSF_NORMAL, gsfStat(body_fail_gsf.c_str(), &file_size));
    {
      ScopedGsfFile seq;
      ASSERT_EQ(GSF_NORMAL, seq.Open(body_fail_gsf.c_str(), GSF_READONLY));
      EXPECT_EQ(-1, CallOpenIndexDirect(body_fail_gsf, seq.handle(), file_size,
                                        /*initial_swap=*/0,
                                        /*open_gsf_fp=*/true));
      EXPECT_EQ(GSF_RECORD_SIZE_ERROR, gsfIntError());
      ASSERT_EQ(GSF_NORMAL, seq.Close());
    }
  }
}

TEST(GsfIndxTest, TempDirectoryResolutionHonorsGsftmpdirAndTempEnvVars) {
  TempDir dir("temp_resolution");
  absl::Cleanup flush_slot = [&dir]() { FlushFileTableSlot(dir); };

  auto run_index_create = [&](std::string_view tag, bool expect_success) {
    const std::filesystem::path gsf_path =
        dir.file(std::format("tmp_res_{}.gsf", tag));
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_CREATE));
      ASSERT_NO_FATAL_FAILURE(
          WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 1));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    ScopedGsfFile gsf;
    if (expect_success) {
      ASSERT_EQ(GSF_NORMAL, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
      EXPECT_EQ(1, gsfGetNumberRecords(gsf.handle(), GSF_RECORD_COMMENT));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    } else {
      EXPECT_EQ(-1, gsf.Open(gsf_path.c_str(), GSF_READONLY_INDEX));
      EXPECT_EQ(GSF_INDEX_FILE_OPEN_ERROR, gsfIntError());
    }
  };

  // 1. GSFTMPDIR points to a valid TempDir (TEMP unset): succeeds and cleans up.
  {
    TempDir gsf_tmp("gsftmp_valid");
    ScopedEnvVar gsf_env("GSFTMPDIR", gsf_tmp.string());
    ScopedEnvVar tmp_env("TEMP", std::nullopt);
    ASSERT_NO_FATAL_FAILURE(run_index_create("gsftmp_valid", true));
    EXPECT_TRUE(std::filesystem::is_empty(gsf_tmp.path()));
  }

  // 2. GSFTMPDIR unset, TEMP points to a valid TempDir: succeeds and cleans up.
  {
    TempDir alt_tmp("temp_env_valid");
    ScopedEnvVar gsf_env("GSFTMPDIR", std::nullopt);
    ScopedEnvVar tmp_env("TEMP", alt_tmp.string());
    ASSERT_NO_FATAL_FAILURE(run_index_create("temp_valid", true));
    EXPECT_TRUE(std::filesystem::is_empty(alt_tmp.path()));
  }

  // 3. Prove both GSFTMPDIR and TEMP (with GSFTMPDIR unset) are actually used
  // without falling back to `/tmp` by pointing each at a 0555 TempDir.
  if (geteuid() != 0) {
    {
      TempDir ro_gsf_tmp("gsftmp_ro");
      ASSERT_EQ(0, chmod(ro_gsf_tmp.c_str(), 0555));
      absl::Cleanup restore_perm = [&ro_gsf_tmp]() {
        chmod(ro_gsf_tmp.c_str(), 0755);
      };
      ScopedEnvVar gsf_env("GSFTMPDIR", ro_gsf_tmp.string());
      ScopedEnvVar tmp_env("TEMP", std::nullopt);
      ASSERT_NO_FATAL_FAILURE(run_index_create("gsftmp_ro", false));
    }
    {
      TempDir ro_alt_tmp("temp_env_ro");
      ASSERT_EQ(0, chmod(ro_alt_tmp.c_str(), 0555));
      absl::Cleanup restore_perm = [&ro_alt_tmp]() {
        chmod(ro_alt_tmp.c_str(), 0755);
      };
      ScopedEnvVar gsf_env("GSFTMPDIR", std::nullopt);
      ScopedEnvVar tmp_env("TEMP", ro_alt_tmp.string());
      ASSERT_NO_FATAL_FAILURE(run_index_create("temp_ro", false));
    }
  }
}

TEST(GsfIndxTest, CloseTellAndTempFileFailurePaths) {
  TempDir dir("close_tell_temp_failures");
  absl::Cleanup flush_slot = [&dir]() { FlushFileTableSlot(dir); };
  ScopedEnvVar env("GSFTMPDIR", dir.string());

  // 1. gsfCloseIndex reports GSF_FILE_CLOSE_ERROR when fclose returns non-zero
  // (`gsf_indx.c:2059-2060`).
  {
    const std::filesystem::path close_err_path = dir.file("close_err.nsf");
    FILE* fp = std::fopen(close_err_path.c_str(), "wb");
    ASSERT_NE(nullptr, fp);
    ASSERT_EQ(1u, std::fwrite("x", 1, 1, fp));
    ASSERT_EQ(0, ::close(::fileno(fp)));
    GSF_FILE_TABLE ft{};
    std::memset(&ft, 0, sizeof(ft));
    ft.index_data.fp = fp;
    gsfError = GSF_NORMAL;
    EXPECT_EQ(-1, gsfCloseIndex(&ft));
    EXPECT_EQ(GSF_FILE_CLOSE_ERROR, gsfIntError());
  }

  // 2. gsfCreateIndexFile reports GSF_FILE_TELL_ERROR (`gsf_indx.c:509-510`)
  // when ft->fp is an unseekable pipe. Note: gsf_indx.c:476 already closed
  // ft->index_data.fp before line 507, so cleanup must not call gsfCloseIndex.
  {
    int pipefd[2] = {-1, -1};
    ASSERT_EQ(0, ::pipe(pipefd));
    ::close(pipefd[1]);
    FILE* pipe_fp = ::fdopen(pipefd[0], "rb");
    ASSERT_NE(nullptr, pipe_fp);
    absl::Cleanup close_pipe = [&pipe_fp]() {
      if (pipe_fp != nullptr) {
        std::fclose(pipe_fp);
      }
    };

    GSF_FILE_TABLE ft{};
    std::memset(&ft, 0, sizeof(ft));
    ft.file_size = 100;
    ft.fp = pipe_fp;
    gsfError = GSF_NORMAL;
    EXPECT_EQ(-1, gsfOpenIndex(dir.file("tell_create.gsf").c_str(), 1, &ft));
    EXPECT_EQ(GSF_FILE_TELL_ERROR, gsfIntError());
  }

  // 3. gsfCreateIndexFile fails `open_temp_file(GSF_RECORD_SWATH_BATHYMETRY_PING)`
  // when `temp[0]` was skipped (`scales_read == 0`, `gsf_indx.c:597-598`).
  {
    const std::filesystem::path ping_gsf = dir.file("create_temp2_fail.gsf");
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(ping_gsf.c_str(), GSF_CREATE));
      ASSERT_NO_FATAL_FAILURE(
          WritePingRecord(gsf.handle(), 1700000100, 100, /*offset=*/0));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    long long file_size = 0;
    ASSERT_EQ(GSF_NORMAL, gsfStat(ping_gsf.c_str(), &file_size));

    const std::filesystem::path blocked_dir = dir.file(
        std::format("{:05d}{:02d}.ndx", getpid(),
                    static_cast<int>(GSF_RECORD_SWATH_BATHYMETRY_PING)));
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directory(blocked_dir, ec))
        << ec.message();
    absl::Cleanup rm_blocked = [&blocked_dir]() {
      std::error_code rm_ec;
      std::filesystem::remove_all(blocked_dir, rm_ec);
    };

    ScopedGsfFile seq;
    ASSERT_EQ(GSF_NORMAL, seq.Open(ping_gsf.c_str(), GSF_READONLY));
    FILE* gsf_fp = std::fopen(ping_gsf.c_str(), "rb");
    ASSERT_NE(nullptr, gsf_fp);
    absl::Cleanup close_fp = [&gsf_fp]() {
      if (gsf_fp != nullptr) {
        std::fclose(gsf_fp);
      }
    };

    GSF_FILE_TABLE ft{};
    std::memset(&ft, 0, sizeof(ft));
    ft.file_size = file_size;
    ft.fp = gsf_fp;
    ft.scales_read = 0;
    gsfError = GSF_NORMAL;
    EXPECT_EQ(-1, gsfOpenIndex(ping_gsf.c_str(), seq.handle(), &ft));
    EXPECT_EQ(GSF_OPEN_TEMP_FILE_FAILED, gsfIntError());
    ASSERT_EQ(GSF_NORMAL, seq.Close());
  }

  // 4. gsfCreateIndexFile fails its second `fopen(ndx_file, "wb+")`
  // (`gsf_indx.c:893-894`) when a progress callback creates a directory at
  // `ndx_file` while reading a second GSF_RECORD_HEADER (which opens no temp
  // files).
  {
    const std::filesystem::path hdr_gsf = dir.file("create_second_fopen.gsf");
    const std::filesystem::path hdr_nsf = IndexPathForGsf(hdr_gsf);
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(hdr_gsf.c_str(), GSF_CREATE));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    const std::vector<uint8_t> header_rec = ReadFileBytes(hdr_gsf);
    AppendRawBytes(hdr_gsf, header_rec);
    long long file_size = 0;
    ASSERT_EQ(GSF_NORMAL, gsfStat(hdr_gsf.c_str(), &file_size));

    g_mkdir_on_progress_path = &hdr_nsf;
    gsf_register_progress_callback(&CreateDirOnReadProgress);
    absl::Cleanup reset_cb = [&hdr_nsf]() {
      gsf_register_progress_callback(nullptr);
      g_mkdir_on_progress_path = nullptr;
      std::error_code rm_ec;
      std::filesystem::remove_all(hdr_nsf, rm_ec);
    };

    ScopedGsfFile seq;
    ASSERT_EQ(GSF_NORMAL, seq.Open(hdr_gsf.c_str(), GSF_READONLY));
    FILE* gsf_fp = std::fopen(hdr_gsf.c_str(), "rb");
    ASSERT_NE(nullptr, gsf_fp);
    absl::Cleanup close_fp = [&gsf_fp]() {
      if (gsf_fp != nullptr) {
        std::fclose(gsf_fp);
      }
    };

    GSF_FILE_TABLE ft{};
    std::memset(&ft, 0, sizeof(ft));
    ft.file_size = file_size;
    ft.fp = gsf_fp;
    gsfError = GSF_NORMAL;
    EXPECT_EQ(-1, gsfOpenIndex(hdr_gsf.c_str(), seq.handle(), &ft));
    EXPECT_EQ(GSF_INDEX_FILE_OPEN_ERROR, gsfIntError());
    EXPECT_EQ(nullptr, ft.index_data.fp);
    ASSERT_EQ(GSF_NORMAL, seq.Close());
  }

  // 5. gsfAppendIndexFile reports GSF_FILE_TELL_ERROR (`gsf_indx.c:1293-1304`)
  // and cleans up opened temp files when `ft->fp` is an unseekable pipe.
  {
    const std::filesystem::path tell_app_gsf = dir.file("tell_append.gsf");
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(tell_app_gsf.c_str(), GSF_CREATE));
      ASSERT_NO_FATAL_FAILURE(
          WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 1));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL,
                gsf.Open(tell_app_gsf.c_str(), GSF_READONLY_INDEX));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    FlushFileTableSlot(dir);
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(tell_app_gsf.c_str(), GSF_APPEND));
      ASSERT_NO_FATAL_FAILURE(
          WriteRecordOfType(gsf.handle(), GSF_RECORD_COMMENT, 2));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    long long file_size = 0;
    ASSERT_EQ(GSF_NORMAL, gsfStat(tell_app_gsf.c_str(), &file_size));

    int pipefd[2] = {-1, -1};
    ASSERT_EQ(0, ::pipe(pipefd));
    ::close(pipefd[1]);
    FILE* pipe_fp = ::fdopen(pipefd[0], "rb");
    ASSERT_NE(nullptr, pipe_fp);
    absl::Cleanup close_pipe = [&pipe_fp]() {
      if (pipe_fp != nullptr) {
        std::fclose(pipe_fp);
      }
    };

    ScopedGsfFile seq;
    ASSERT_EQ(GSF_NORMAL, seq.Open(tell_app_gsf.c_str(), GSF_READONLY));
    GSF_FILE_TABLE ft{};
    std::memset(&ft, 0, sizeof(ft));
    ft.file_size = file_size;
    ft.fp = pipe_fp;
    absl::Cleanup cleanup_append = [&ft]() {
      if (ft.index_data.fp != nullptr) {
        gsfCloseIndex(&ft);
      }
      std::free(ft.index_data.scale_factor_addr);
      ft.index_data.scale_factor_addr = nullptr;
    };
    gsfError = GSF_NORMAL;
    EXPECT_EQ(-1, gsfOpenIndex(tell_app_gsf.c_str(), seq.handle(), &ft));
    EXPECT_EQ(GSF_FILE_TELL_ERROR, gsfIntError());
    ASSERT_EQ(GSF_NORMAL, seq.Close());
  }

  // 6. gsfAppendIndexFile per-record-type `open_temp_file` failure branches
  // (`gsf_indx.c:1335-1632`). Skipped under ASan/LSan because upstream
  // returns -1 without closing the initial type's open `temp[]` FILE*.
#if !defined(ADDRESS_SANITIZER) && !defined(ABSL_HAVE_ADDRESS_SANITIZER) && \
    !defined(LEAK_SANITIZER) && !defined(ABSL_HAVE_LEAK_SANITIZER)
  struct AppendTempFailCase {
    unsigned int initial_id;
    unsigned int append_id;
    int blocked_temp_id;
    int scales_read;
  };
  constexpr auto kAppendTempFailCases = std::to_array<AppendTempFailCase>({
      {.initial_id = GSF_RECORD_COMMENT,
       .append_id = GSF_RECORD_SWATH_BATHY_SUMMARY,
       .blocked_temp_id = GSF_RECORD_SWATH_BATHY_SUMMARY,
       .scales_read = 0},
      {.initial_id = GSF_RECORD_SOUND_VELOCITY_PROFILE,
       .append_id = GSF_RECORD_SWATH_BATHYMETRY_PING,
       .blocked_temp_id = 0,
       .scales_read = 1},
      {.initial_id = GSF_RECORD_SOUND_VELOCITY_PROFILE,
       .append_id = GSF_RECORD_SWATH_BATHYMETRY_PING,
       .blocked_temp_id = GSF_RECORD_SWATH_BATHYMETRY_PING,
       .scales_read = 0},
      {.initial_id = GSF_RECORD_COMMENT,
       .append_id = GSF_RECORD_SOUND_VELOCITY_PROFILE,
       .blocked_temp_id = GSF_RECORD_SOUND_VELOCITY_PROFILE,
       .scales_read = 0},
      {.initial_id = GSF_RECORD_COMMENT,
       .append_id = GSF_RECORD_PROCESSING_PARAMETERS,
       .blocked_temp_id = GSF_RECORD_PROCESSING_PARAMETERS,
       .scales_read = 0},
      {.initial_id = GSF_RECORD_COMMENT,
       .append_id = GSF_RECORD_SENSOR_PARAMETERS,
       .blocked_temp_id = GSF_RECORD_SENSOR_PARAMETERS,
       .scales_read = 0},
      {.initial_id = GSF_RECORD_SOUND_VELOCITY_PROFILE,
       .append_id = GSF_RECORD_COMMENT,
       .blocked_temp_id = GSF_RECORD_COMMENT,
       .scales_read = 0},
      {.initial_id = GSF_RECORD_COMMENT,
       .append_id = GSF_RECORD_HISTORY,
       .blocked_temp_id = GSF_RECORD_HISTORY,
       .scales_read = 0},
      {.initial_id = GSF_RECORD_COMMENT,
       .append_id = GSF_RECORD_NAVIGATION_ERROR,
       .blocked_temp_id = GSF_RECORD_NAVIGATION_ERROR,
       .scales_read = 0},
      {.initial_id = GSF_RECORD_COMMENT,
       .append_id = GSF_RECORD_HV_NAVIGATION_ERROR,
       .blocked_temp_id = GSF_RECORD_HV_NAVIGATION_ERROR,
       .scales_read = 0},
      {.initial_id = GSF_RECORD_COMMENT,
       .append_id = GSF_RECORD_ATTITUDE,
       .blocked_temp_id = GSF_RECORD_ATTITUDE,
       .scales_read = 0},
  });

  for (std::size_t idx = 0; idx < kAppendTempFailCases.size(); ++idx) {
    const auto& tc = kAppendTempFailCases[idx];
    SCOPED_TRACE(::testing::Message()
                 << "idx=" << idx << " append_id=" << tc.append_id
                 << " blocked_temp_id=" << tc.blocked_temp_id);
    const std::filesystem::path app_gsf =
        dir.file(std::format("append_temp_fail_{}.gsf", idx));
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(app_gsf.c_str(), GSF_CREATE));
      ASSERT_NO_FATAL_FAILURE(WriteRecordOfType(gsf.handle(), tc.initial_id, 1));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(app_gsf.c_str(), GSF_READONLY_INDEX));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    FlushFileTableSlot(dir);
    {
      ScopedGsfFile gsf;
      ASSERT_EQ(GSF_NORMAL, gsf.Open(app_gsf.c_str(), GSF_APPEND));
      ASSERT_NO_FATAL_FAILURE(WriteRecordOfType(gsf.handle(), tc.append_id, 2));
      ASSERT_EQ(GSF_NORMAL, gsf.Close());
    }
    long long file_size = 0;
    ASSERT_EQ(GSF_NORMAL, gsfStat(app_gsf.c_str(), &file_size));

    const std::filesystem::path blocked_dir =
        dir.file(std::format("{:05d}{:02d}.ndx", getpid(), tc.blocked_temp_id));
    const std::filesystem::path leaked_tmp = dir.file(
        std::format("{:05d}{:02d}.ndx", getpid(), static_cast<int>(tc.initial_id)));
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directory(blocked_dir, ec))
        << ec.message();
    absl::Cleanup cleanup_tmp = [&blocked_dir, &leaked_tmp]() {
      std::error_code rm_ec;
      std::filesystem::remove_all(blocked_dir, rm_ec);
      std::filesystem::remove(leaked_tmp, rm_ec);
    };

    ScopedGsfFile seq;
    ASSERT_EQ(GSF_NORMAL, seq.Open(app_gsf.c_str(), GSF_READONLY));
    FILE* gsf_fp = std::fopen(app_gsf.c_str(), "rb");
    ASSERT_NE(nullptr, gsf_fp);
    GSF_FILE_TABLE ft{};
    std::memset(&ft, 0, sizeof(ft));
    ft.file_size = file_size;
    ft.fp = gsf_fp;
    ft.scales_read = tc.scales_read;
    absl::Cleanup cleanup_ft = [&ft, &gsf_fp]() {
      if (ft.index_data.fp != nullptr) {
        gsfCloseIndex(&ft);
      }
      if (gsf_fp != nullptr) {
        std::fclose(gsf_fp);
      }
      std::free(ft.index_data.scale_factor_addr);
      ft.index_data.scale_factor_addr = nullptr;
    };

    gsfError = GSF_NORMAL;
    EXPECT_EQ(-1, gsfOpenIndex(app_gsf.c_str(), seq.handle(), &ft));
    EXPECT_EQ(GSF_OPEN_TEMP_FILE_FAILED, gsfIntError());
    ASSERT_EQ(GSF_NORMAL, seq.Close());
    FlushFileTableSlot(dir);
  }
#endif
}

// TODO(schwehr): Remaining uncovered lines in src/gsf_indx.c are the
// SwapLongLong(count > 0) stack-OOB path (line 2167) and swapped-endian TOC
// loops (lines 360..417, 1126..1214, 1744..1835), `/tmp` fallback paths
// (lines 1886, 1894, 1942-1954), dead v01 TOC 32-bit offset reads (lines
// 378-381), OOM calloc/realloc checks, and second-ftell failures (lines
// 857, 1656).

}  // namespace
}  // namespace gsf
