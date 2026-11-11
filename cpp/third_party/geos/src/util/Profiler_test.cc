// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/profiler.h"

#include <chrono>
#include <iostream>
#include <locale>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

class ScopedCerrRedirect {
 public:
  explicit ScopedCerrRedirect(std::ostream& target)
      : previous_(std::cerr.rdbuf(target.rdbuf())) {}

  ScopedCerrRedirect(const ScopedCerrRedirect&) = delete;
  ScopedCerrRedirect& operator=(const ScopedCerrRedirect&) = delete;

  ~ScopedCerrRedirect() { std::cerr.rdbuf(previous_); }

 private:
  std::streambuf* previous_;
};

TEST(ProfileTest, InitialStateBeforeTimings) {
  geos::util::Profile prof("initial");

  EXPECT_EQ(prof.name, "initial");
  EXPECT_EQ(prof.getNumTimings(), 0u);
  EXPECT_DOUBLE_EQ(prof.getTot(), 0.0);
  EXPECT_EQ(prof.getTotFormatted(), "0");
}

TEST(ProfileTest, SingleTimingAndFormattedTotal) {
  geos::util::Profile prof("single");

  prof.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  prof.stop();

  EXPECT_EQ(prof.getNumTimings(), 1u);
  EXPECT_GE(prof.getTot(), 0.0);
  EXPECT_DOUBLE_EQ(prof.getMin(), prof.getTot());
  EXPECT_DOUBLE_EQ(prof.getMax(), prof.getTot());
  EXPECT_DOUBLE_EQ(prof.getAvg(), prof.getTot());

  std::stringstream expected;
  expected.imbue(std::locale(""));
  expected << std::fixed
           << static_cast<geos::util::Profile::timeunit::rep>(prof.getTot());
  EXPECT_EQ(prof.getTotFormatted(), expected.str());
}

TEST(ProfileTest, MultipleTimingsTrackMinMaxAvgAndStream) {
  geos::util::Profile prof("multi");

  // Run 1: medium duration sets initial min and max.
  prof.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  prof.stop();

  // Run 2: longer duration exercises the elapsed > max branch.
  prof.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(12));
  prof.stop();

  // Run 3: immediate stop exercises the elapsed < min branch.
  prof.start();
  prof.stop();

  EXPECT_EQ(prof.getNumTimings(), 3u);
  EXPECT_LE(prof.getMin(), prof.getAvg());
  EXPECT_LE(prof.getAvg(), prof.getMax());
  EXPECT_LE(prof.getMin(), prof.getMax());
  EXPECT_NEAR(prof.getAvg() * 3.0, prof.getTot(), 1e-6);

  std::ostringstream oss;
  oss << prof;

  std::ostringstream expected;
  expected << " num:" << prof.getNumTimings() << " min:" << prof.getMin()
           << " max:" << prof.getMax() << " avg:" << prof.getAvg()
           << " tot:" << prof.getTot() << " [" << prof.name << "]";
  EXPECT_EQ(oss.str(), expected.str());
}

TEST(ProfilerTest, EmptyProfilerStreamsEmptyString) {
  geos::util::Profiler profiler;
  std::ostringstream oss;
  oss << profiler;
  EXPECT_TRUE(oss.str().empty());
}

TEST(ProfilerTest, StartStopGetAndStreamMultipleProfiles) {
  geos::util::Profiler profiler;

  geos::util::Profile* alpha = profiler.get("alpha");
  ASSERT_NE(alpha, nullptr);
  EXPECT_EQ(alpha->name, "alpha");
  // Second lookup returns the existing Profile without recreating it.
  EXPECT_EQ(profiler.get("alpha"), alpha);

  profiler.start("alpha");
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  profiler.stop("alpha");
  EXPECT_EQ(alpha->getNumTimings(), 1u);

  profiler.start("beta");
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  profiler.stop("beta");

  geos::util::Profile* beta = profiler.get("beta");
  ASSERT_NE(beta, nullptr);
  EXPECT_EQ(beta->getNumTimings(), 1u);

  std::ostringstream oss;
  oss << profiler;

  std::ostringstream expected;
  expected << *alpha << '\n' << *beta << '\n';
  EXPECT_EQ(oss.str(), expected.str());
}

TEST(ProfilerTest, StopUnknownProfileLogsToCerr) {
  geos::util::Profiler profiler;
  std::ostringstream captured_cerr;
  {
    ScopedCerrRedirect redirect(captured_cerr);
    profiler.stop("missing_task");
  }

  EXPECT_EQ(captured_cerr.str(), "missing_task: no such Profile started");
  EXPECT_TRUE(profiler.profs.empty());
}

TEST(ProfilerTest, SingletonInstanceIsConsistent) {
  geos::util::Profiler* inst1 = geos::util::Profiler::instance();
  geos::util::Profiler* inst2 = geos::util::Profiler::instance();

  ASSERT_NE(inst1, nullptr);
  EXPECT_EQ(inst1, inst2);

  constexpr char kTaskName[] = "geos_google_profiler_singleton_test";
  inst1->start(kTaskName);
  inst1->stop(kTaskName);

  geos::util::Profile* prof = inst1->get(kTaskName);
  ASSERT_NE(prof, nullptr);
  EXPECT_EQ(prof->name, kTaskName);
  EXPECT_GE(prof->getNumTimings(), 1u);
}

}  // namespace
}  // namespace geos_google
