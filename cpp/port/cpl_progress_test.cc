// Tests for progress tracking functions.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_progress.cpp
//
// Copyright 2014 Google Inc. All Rights Reserved.
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
// GDALTermProgress keeps its state in function-local statics and reads the
// wall clock directly, so the estimated-remaining-time branch (which also
// requires stdout to be a TTY) is not unit-testable without a seam in
// upstream GDAL. Only the clock-independent behaviour is covered here.

#include "third_party/gdal/port/cpl_progress.h"

#include <string>

#include "testing/base/public/gunit.h"

namespace autotest2 {
namespace {

// Tests print progress to stdout.
TEST(GDALProgress, TermProgress) {
  // GDALTermProgress never returns FALSE.
  for (double completed = 0.0; completed <= 2.0; completed += 0.1) {
    ASSERT_EQ(TRUE, GDALTermProgress(completed, "", nullptr));
  }
}

// Verifies the text written to stdout for a full, fast progress run.
TEST(GDALProgress, TermProgressOutput) {
  // Finish any run left over from an earlier test so that the 0.0 call below
  // starts a fresh run and resets the internal static state.
  ASSERT_EQ(TRUE, GDALTermProgress(1.0, "", nullptr));

  testing::internal::CaptureStdout();
  EXPECT_EQ(TRUE, GDALTermProgress(0.0, "", nullptr));
  EXPECT_EQ(TRUE, GDALTermProgress(0.25, "", nullptr));
  // Going backwards before completion is ignored.
  EXPECT_EQ(TRUE, GDALTermProgress(0.1, "", nullptr));
  EXPECT_EQ(TRUE, GDALTermProgress(1.0, "", nullptr));
  const std::string output = testing::internal::GetCapturedStdout();

  // The run completes in well under 5 seconds, so no elapsed time is printed.
  EXPECT_EQ("0...10...20...30...40...50...60...70...80...90...100 - done.\n",
            output);
}

// Tests the dummy progress function that does nothing.
TEST(GDALProgress, UseDummy) {
  // GDALDummyProgress never returns FALSE.
  for (double completed = -6.0; completed <= 12; completed += 1) {
    ASSERT_EQ(TRUE, GDALDummyProgress(completed, "", nullptr));
  }
}

// A progress function that quits at 1.0.
int CustomProgress(const double complete, const char* message, void* arg) {
  if (complete > 1.0)
    return FALSE;
  else
    return TRUE;
}

// Tests using a custom progress handler that will request termination.
TEST(GDALProgress, ScaledProgressSimple) {
  void* progress_data = GDALCreateScaledProgress(0, 1, CustomProgress, nullptr);
  ASSERT_NE(nullptr, progress_data);

  double completed = 0;
  for (; GDALScaledProgress(completed, "", progress_data); completed += 0.1) {
  }
  ASSERT_DOUBLE_EQ(1.1, completed);

  GDALDestroyScaledProgress(progress_data);
}

// Records the arguments GDALScaledProgress forwards to the wrapped callback.
struct RecordedProgress {
  double complete = -1.0;
  std::string message;
};

int RecordProgress(double complete, const char* message, void* arg) {
  auto* recorded = static_cast<RecordedProgress*>(arg);
  recorded->complete = complete;
  recorded->message = message == nullptr ? "" : message;
  return TRUE;
}

// Verifies the linear mapping of [0, 1] onto [dfMin, dfMax].
TEST(GDALProgress, ScaledProgressMapping) {
  RecordedProgress recorded;
  void* progress_data =
      GDALCreateScaledProgress(0.25, 0.75, RecordProgress, &recorded);
  ASSERT_NE(nullptr, progress_data);

  EXPECT_EQ(TRUE, GDALScaledProgress(0.0, "start", progress_data));
  EXPECT_DOUBLE_EQ(0.25, recorded.complete);
  EXPECT_EQ("start", recorded.message);

  EXPECT_EQ(TRUE, GDALScaledProgress(0.5, "middle", progress_data));
  EXPECT_DOUBLE_EQ(0.5, recorded.complete);
  EXPECT_EQ("middle", recorded.message);

  EXPECT_EQ(TRUE, GDALScaledProgress(1.0, "end", progress_data));
  EXPECT_DOUBLE_EQ(0.75, recorded.complete);
  EXPECT_EQ("end", recorded.message);

  GDALDestroyScaledProgress(progress_data);
}

TEST(GDALProgress, ScaledProgressEdgeCases) {
  // Null or dummy progress returns nullptr.
  EXPECT_EQ(nullptr, GDALCreateScaledProgress(0.0, 1.0, nullptr, nullptr));
  EXPECT_EQ(nullptr,
            GDALCreateScaledProgress(0.0, 1.0, GDALDummyProgress, nullptr));

  // Passing nullptr info to GDALScaledProgress returns TRUE.
  EXPECT_EQ(TRUE, GDALScaledProgress(0.5, "msg", nullptr));

  // dfMin == dfMax adjusts dfMax to dfMin + 0.01.
  RecordedProgress recorded;
  void* progress_data =
      GDALCreateScaledProgress(0.5, 0.5, RecordProgress, &recorded);
  ASSERT_NE(nullptr, progress_data);
  EXPECT_EQ(TRUE, GDALScaledProgress(1.0, "", progress_data));
  EXPECT_NEAR(0.51, recorded.complete, 1e-6);
  GDALDestroyScaledProgress(progress_data);

  // Destroying a nullptr handle is a no-op.
  GDALDestroyScaledProgress(nullptr);
}

}  // namespace
}  // namespace autotest2
