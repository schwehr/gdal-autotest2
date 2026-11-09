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
// Tests internal timer, clock, bandwidth formatting, and human-readable time
// formatting routines (third_party/hdf5/src/H5timer.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5timer.c

#ifndef NDEBUG
#define H5TIMER_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#include "third_party/hdf5/src/H5timer.h"
}  // extern "C"

#ifdef H5TIMER_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TIMER_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

std::string FormatTimeString(double seconds) {
  std::unique_ptr<char, decltype(&std::free)> raw(
      H5_timer_get_time_string(seconds), &std::free);
  if (!raw) {
    return {};
  }
  return std::string(raw.get());
}

TEST(H5TimerTest, BandwidthFormatsAllScaleRangesAndOverflowFallback) {
  char buf[64]{};

  H5_bandwidth(buf, sizeof(buf), 100.0, 0.0);
  EXPECT_STREQ(buf, "       NaN");

  H5_bandwidth(buf, sizeof(buf), 100.0, -2.0);
  EXPECT_STREQ(buf, "       NaN");

  H5_bandwidth(buf, sizeof(buf), 0.0, 1.0);
  EXPECT_STREQ(buf, "0.000  B/s");

  H5_bandwidth(buf, sizeof(buf), 0.5, 1.0);
  EXPECT_THAT(buf, HasSubstr("5.0000e-01"));

  struct ScaleCase {
    double bytes;
    std::string_view expected;
  };
  const auto kScaleCases = std::to_array<ScaleCase>({
      {.bytes = 1.0, .expected = "1.000  B/s"},
      {.bytes = 4.0 * static_cast<double>(H5_KB), .expected = "4.000 kB/s"},
      {.bytes = 1.234 * static_cast<double>(H5_MB), .expected = "1.234 MB/s"},
      {.bytes = 12.34 * static_cast<double>(H5_GB), .expected = "12.34 GB/s"},
      {.bytes = 123.4 * static_cast<double>(H5_TB), .expected = "123.4 TB/s"},
      {.bytes = 512.0 * static_cast<double>(H5_PB), .expected = "512.0 PB/s"},
  });
  for (const auto& tc : kScaleCases) {
    std::memset(buf, 0, sizeof(buf));
    H5_bandwidth(buf, sizeof(buf), tc.bytes, 1.0);
    EXPECT_EQ(std::string_view(buf), tc.expected);
  }

  // >= H5_EB fits in 10 chars at .4e; >= 1e100 spills to 11 chars and
  // exercises the .3e fallback.
  H5_bandwidth(buf, sizeof(buf), 2.0 * static_cast<double>(H5_EB), 1.0);
  EXPECT_EQ(std::strlen(buf), 10U);
  EXPECT_THAT(buf, HasSubstr("e+18"));

  H5_bandwidth(buf, sizeof(buf), 1.0e106, 1.0);
  EXPECT_STREQ(buf, "1.000e+106");
}

TEST(H5TimerTest, ClockGettersArePositiveAndMonotonic) {
  const time_t epoch_before = std::time(nullptr);
  const time_t now_sec = H5_now();
  const time_t epoch_after = std::time(nullptr);
  EXPECT_GE(now_sec, epoch_before - 1);
  EXPECT_LE(now_sec, epoch_after + 1);

  const uint64_t usec0 = H5_now_usec();
  const double sec0 = H5_get_time();
  EXPECT_GT(usec0, 0ULL);
  EXPECT_GT(sec0, 0.0);

  H5_nanosleep(2000000ULL);
  const uint64_t usec1 = H5_now_usec();
  const double sec1 = H5_get_time();
  EXPECT_GE(usec1, usec0);
  EXPECT_GE(sec1, sec0);
}

TEST(H5TimerTest, TimerLifecycleAccumulatesAndResets) {
  H5_timer_t timer{};
  std::memset(&timer, 0x5a, sizeof(timer));

  ASSERT_EQ(H5_timer_init(&timer), 0);
  EXPECT_FALSE(timer.is_running);

  H5_timevals_t times{};
  ASSERT_EQ(H5_timer_get_times(timer, &times), 0);
  EXPECT_DOUBLE_EQ(times.elapsed, 0.0);
  EXPECT_DOUBLE_EQ(times.user, 0.0);
  EXPECT_DOUBLE_EQ(times.system, 0.0);

  ASSERT_EQ(H5_timer_get_total_times(timer, &times), 0);
  EXPECT_DOUBLE_EQ(times.elapsed, 0.0);

  // Interval 1: inspect while running, then stop.
  ASSERT_EQ(H5_timer_start(&timer), 0);
  EXPECT_TRUE(timer.is_running);
  H5_nanosleep(2000000ULL);

  H5_timevals_t running_interval{};
  H5_timevals_t running_total{};
  ASSERT_EQ(H5_timer_get_times(timer, &running_interval), 0);
  ASSERT_EQ(H5_timer_get_total_times(timer, &running_total), 0);
  EXPECT_GE(running_interval.elapsed, 0.0);
  EXPECT_GE(running_total.elapsed, running_interval.elapsed);

  ASSERT_EQ(H5_timer_stop(&timer), 0);
  EXPECT_FALSE(timer.is_running);

  H5_timevals_t interval1{};
  ASSERT_EQ(H5_timer_get_times(timer, &interval1), 0);
  EXPECT_GE(interval1.elapsed, 0.0);
  EXPECT_GE(interval1.user, 0.0);
  EXPECT_GE(interval1.system, 0.0);

  // Interval 2: totals accumulate across sessions while final_interval tracks
  // only the latest session.
  ASSERT_EQ(H5_timer_start(&timer), 0);
  H5_nanosleep(2000000ULL);
  ASSERT_EQ(H5_timer_stop(&timer), 0);

  H5_timevals_t interval2{};
  H5_timevals_t total2{};
  ASSERT_EQ(H5_timer_get_times(timer, &interval2), 0);
  ASSERT_EQ(H5_timer_get_total_times(timer, &total2), 0);
  EXPECT_NEAR(total2.elapsed, interval1.elapsed + interval2.elapsed, 1e-12);

  // Re-initialization clears all accumulated intervals and state.
  ASSERT_EQ(H5_timer_init(&timer), 0);
  ASSERT_EQ(H5_timer_get_total_times(timer, &total2), 0);
  EXPECT_DOUBLE_EQ(total2.elapsed, 0.0);
}

TEST(H5TimerTest, TimeStringFormatsAllRanges) {
  struct FormatCase {
    double seconds;
    std::string_view expected;
  };
  constexpr auto kCases = std::to_array<FormatCase>({
      {.seconds = -1.0, .expected = "N/A"},
      {.seconds = 0.0, .expected = "0.0 s"},
      {.seconds = 5.0e-7, .expected = "500 ns"},
      {.seconds = 2.5e-5, .expected = "25.0 us"},
      {.seconds = 0.125, .expected = "125.0 ms"},
      {.seconds = 12.34, .expected = "12.34 s"},
      {.seconds = 125.0, .expected = "2 m 5 s"},
      {.seconds = 3665.0, .expected = "1 h 1 m 5 s"},
      {.seconds = 90061.0, .expected = "1 d 1 h 1 m 1 s"},
  });

  for (const auto& tc : kCases) {
    EXPECT_EQ(FormatTimeString(tc.seconds), tc.expected)
        << "seconds=" << tc.seconds;
  }
}

}  // namespace
}  // namespace hdf5
