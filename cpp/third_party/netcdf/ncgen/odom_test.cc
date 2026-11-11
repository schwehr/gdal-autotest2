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

// Unit tests for //third_party/netcdf/ncgen/odom.c.
//
// Exercises multi-dimensional odometer construction, subodometers, iteration,
// linear offset computation, formatting, and invalid-argument assertions.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/odom.c

#if !defined(ADDRESS_SANITIZER) && !defined(MEMORY_SANITIZER) && \
    !defined(THREAD_SANITIZER)
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#endif

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"

namespace netcdf {
namespace {

Symbol MakeDimSymbol(size_t declsize) {
  Symbol sym{};
  sym.objectclass = NC_DIM;
  sym.dim.declsize = declsize;
  return sym;
}

class DeathTestStyleGuard {
 public:
  DeathTestStyleGuard() : prev_(GTEST_FLAG_GET(death_test_style)) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
  }
  ~DeathTestStyleGuard() { GTEST_FLAG_SET(death_test_style, prev_); }

 private:
  std::string prev_;
};

#if !defined(ADDRESS_SANITIZER) && !defined(MEMORY_SANITIZER) && \
    !defined(THREAD_SANITIZER)
sigjmp_buf g_abort_jmp_buf;
volatile sig_atomic_t g_abort_jmp_active = 0;

void HandleAbortForCoverage(int /*sig*/) {
  if (g_abort_jmp_active) {
    siglongjmp(g_abort_jmp_buf, 1);
  }
}

template <typename Fn>
::testing::AssertionResult ExpectAbortInForkedChild(Fn&& fn) {
  return RunInForkedChild([&]() -> bool {
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    struct sigaction sa{};
    sa.sa_handler = HandleAbortForCoverage;
    sigemptyset(&sa.sa_mask);
    struct sigaction prev_sa{};
    if (::sigaction(SIGABRT, &sa, &prev_sa) != 0) {
      return false;
    }
    g_abort_jmp_active = 1;
    if (sigsetjmp(g_abort_jmp_buf, 1) == 0) {
      std::forward<Fn>(fn)();
      g_abort_jmp_active = 0;
      ::sigaction(SIGABRT, &prev_sa, nullptr);
      return false;
    }
    g_abort_jmp_active = 0;
    ::sigaction(SIGABRT, &prev_sa, nullptr);
    return true;
  });
}
#endif

TEST(OdomTest, FullIterationCoversAllLinearOffsetsAndResets) {
  Symbol d0 = MakeDimSymbol(2);
  Symbol d1 = MakeDimSymbol(5);
  Symbol d2 = MakeDimSymbol(3);
  Dimset dimset{};
  dimset.ndims = 3;
  dimset.dimsyms[0] = &d0;
  dimset.dimsyms[1] = &d1;
  dimset.dimsyms[2] = &d2;

  Odometer* odom = newodometer(&dimset, nullptr, nullptr);
  ASSERT_NE(nullptr, odom);
  const absl::Cleanup free_odom([&]() { odometerfree(odom); });

  const size_t* starts = odometerstartvector(odom);
  const size_t* counts = odometercountvector(odom);
  EXPECT_EQ(0UL, starts[0]);
  EXPECT_EQ(0UL, starts[1]);
  EXPECT_EQ(0UL, starts[2]);
  EXPECT_EQ(2UL, counts[0]);
  EXPECT_EQ(5UL, counts[1]);
  EXPECT_EQ(3UL, counts[2]);

  bool saw_offset_19 = false;
  std::vector<size_t> visited_offsets;
  while (odometermore(odom)) {
    const size_t offset = odometeroffset(odom);
    visited_offsets.push_back(offset);
    if (offset == 19) {
      // At flat index 19: (1 * 5 + 1) * 3 + 1 => [1][1][1].
      saw_offset_19 = true;
      EXPECT_STREQ("[1/0..2:2][1/0..5:5][1/0..3:3]", odometerprint(odom));
    }
    const int incremented_dim = odometerincr(odom);
    if (offset == 18) {
      EXPECT_EQ(2, incremented_dim);
    } else if (offset == 20) {
      EXPECT_EQ(1, incremented_dim);
    } else if (offset == 29) {
      EXPECT_EQ(0, incremented_dim);
    }
  }
  EXPECT_TRUE(saw_offset_19);
  ASSERT_EQ(30UL, visited_offsets.size());
  for (size_t i = 0; i < 30; ++i) {
    EXPECT_EQ(i, visited_offsets[i]);
  }
  // odometermore() resets index[0] back to start[0] on exhaustion.
  EXPECT_EQ(0UL, odom->index[0]);
  EXPECT_EQ(0UL, odom->index[1]);
  EXPECT_EQ(0UL, odom->index[2]);

  odometerfree(nullptr);
}

TEST(OdomTest, SubodometerIteratesSliceAndPrintsBrackets) {
  Symbol d0 = MakeDimSymbol(4);
  Symbol d1 = MakeDimSymbol(5);
  Symbol d2 = MakeDimSymbol(6);
  Dimset dimset{};
  dimset.ndims = 3;
  dimset.dimsyms[0] = &d0;
  dimset.dimsyms[1] = &d1;
  dimset.dimsyms[2] = &d2;

  std::array<size_t, 3> start = {1, 2, 1};
  std::array<size_t, 3> count = {2, 2, 3};
  Odometer* odom = newodometer(&dimset, start.data(), count.data());
  ASSERT_NE(nullptr, odom);
  const absl::Cleanup free_odom([&]() { odometerfree(odom); });

  Odometer* sub = newsubodometer(odom, &dimset, 1, 3);
  ASSERT_NE(nullptr, sub);
  const absl::Cleanup free_sub([&]() { odometerfree(sub); });

  EXPECT_STREQ("[2/2..5:2][1/1..6:3]", odometerprint(sub));

  std::vector<size_t> sub_offsets;
  while (odometermore(sub)) {
    sub_offsets.push_back(odometeroffset(sub));
    odometerincr(sub);
  }
  EXPECT_THAT(sub_offsets,
              ::testing::ElementsAre(13UL, 14UL, 15UL, 19UL, 20UL, 21UL));

  // Verify zero-rank formatting path in odometerprint().
  odom->rank = 0;
  EXPECT_STREQ("[]", odometerprint(odom));
}

TEST(OdomTest, ZeroCountSliceResetsWithoutIterating) {
  Symbol d0 = MakeDimSymbol(4);
  Dimset dimset{};
  dimset.ndims = 1;
  dimset.dimsyms[0] = &d0;

  std::array<size_t, 1> start = {2};
  std::array<size_t, 1> count = {0};
  Odometer* odom = newodometer(&dimset, start.data(), count.data());
  ASSERT_NE(nullptr, odom);
  const absl::Cleanup free_odom([&]() { odometerfree(odom); });

  EXPECT_EQ(2UL, odometeroffset(odom));
  EXPECT_STREQ("[2/2..4:0]", odometerprint(odom));
  EXPECT_EQ(0, odometermore(odom));
  EXPECT_EQ(2UL, odom->index[0]);
}

TEST(OdomTest, InvalidArgumentsTriggerAssertionAborts) {
  DeathTestStyleGuard death_guard;

  auto run_all_checks = []() {
    Symbol d0 = MakeDimSymbol(3);
    Symbol d1 = MakeDimSymbol(4);
    Dimset dimset{};
    dimset.ndims = 2;
    dimset.dimsyms[0] = &d0;
    dimset.dimsyms[1] = &d1;

    EXPECT_DEATH(newodometer(nullptr, nullptr, nullptr), "dimset != NULL");
    EXPECT_DEATH(
        {
          Dimset empty{};
          empty.ndims = 0;
          newodometer(&empty, nullptr, nullptr);
        },
        "ndims > 0");
    EXPECT_DEATH(
        {
          Dimset huge{};
          huge.ndims = NC_MAX_VAR_DIMS + 1;
          newodometer(&huge, nullptr, nullptr);
        },
        "NC_MAX_VAR_DIMS");
    std::array<size_t, 2> bad_start = {2, 0};
    std::array<size_t, 2> bad_count = {2, 1};
    EXPECT_DEATH(newodometer(&dimset, bad_start.data(), bad_count.data()),
                 "declsize");

    EXPECT_DEATH(newsubodometer(nullptr, nullptr, 0, 1), "dimset != NULL");
    EXPECT_DEATH(
        {
          Dimset empty{};
          newsubodometer(nullptr, &empty, 0, 1);
        },
        "ndims > 0");
    EXPECT_DEATH(newsubodometer(nullptr, &dimset, 0, 3), "ndims >= stop");
    EXPECT_DEATH(newsubodometer(nullptr, &dimset, 1, 1), "stop > start");
    EXPECT_DEATH(
        {
          Dimset big{};
          big.ndims = NC_MAX_VAR_DIMS + 1;
          newsubodometer(nullptr, &big, -1, NC_MAX_VAR_DIMS);
        },
        "NC_MAX_VAR_DIMS");

    EXPECT_DEATH(
        {
          Odometer zero_rank{};
          zero_rank.origin = &zero_rank;
          zero_rank.rank = 0;
          odometermore(&zero_rank);
        },
        "rank > 0");
    EXPECT_DEATH(
        {
          Odometer zero_rank{};
          zero_rank.origin = &zero_rank;
          zero_rank.rank = 0;
          odometerincr(&zero_rank);
        },
        "rank > 0");
  };
  run_all_checks();

#if !defined(ADDRESS_SANITIZER) && !defined(MEMORY_SANITIZER) && \
    !defined(THREAD_SANITIZER)
  Symbol d0 = MakeDimSymbol(3);
  Symbol d1 = MakeDimSymbol(4);
  Dimset dimset{};
  dimset.ndims = 2;
  dimset.dimsyms[0] = &d0;
  dimset.dimsyms[1] = &d1;

  EXPECT_TRUE(ExpectAbortInForkedChild(
      []() { newodometer(nullptr, nullptr, nullptr); }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    Dimset empty{};
    newodometer(&empty, nullptr, nullptr);
  }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    Dimset huge{};
    huge.ndims = NC_MAX_VAR_DIMS + 1;
    newodometer(&huge, nullptr, nullptr);
  }));
  EXPECT_TRUE(ExpectAbortInForkedChild([&]() {
    std::array<size_t, 2> bad_start = {2, 0};
    std::array<size_t, 2> bad_count = {2, 1};
    newodometer(&dimset, bad_start.data(), bad_count.data());
  }));
  EXPECT_TRUE(ExpectAbortInForkedChild(
      []() { newsubodometer(nullptr, nullptr, 0, 1); }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    Dimset empty{};
    newsubodometer(nullptr, &empty, 0, 1);
  }));
  EXPECT_TRUE(ExpectAbortInForkedChild(
      [&]() { newsubodometer(nullptr, &dimset, 0, 3); }));
  EXPECT_TRUE(ExpectAbortInForkedChild(
      [&]() { newsubodometer(nullptr, &dimset, 1, 1); }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    Dimset big{};
    big.ndims = NC_MAX_VAR_DIMS + 1;
    newsubodometer(nullptr, &big, -1, NC_MAX_VAR_DIMS);
  }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    Odometer zero_rank{};
    zero_rank.origin = &zero_rank;
    zero_rank.rank = 0;
    odometermore(&zero_rank);
  }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    Odometer zero_rank{};
    zero_rank.origin = &zero_rank;
    zero_rank.rank = 0;
    odometerincr(&zero_rank);
  }));
#endif
}

}  // namespace
}  // namespace netcdf
