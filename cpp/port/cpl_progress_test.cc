// Tests for progress tracking functions.
//
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_progress.cpp
//
// Copyright 2014 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// The GDAL progress handler setup does not expose the scaled progress data
// cleanly, so it is difficult to test.

#include "port/cpl_progress.h"

#include "gunit.h"

namespace {

// Tests print progress to stdout.
TEST(GDALProgress, TermProgress) {
  // GDALTermProgress never returns FALSE.
  for (double completed = 0.0; completed <= 2.0; completed += 0.1) {
    ASSERT_EQ(1, GDALTermProgress(completed, "", nullptr));
  }
}

// Tests the dummy progress function that does nothing.
TEST(GDALProgress, UseDummy) {
  // GDALDummyProgress never returns FALSE.
  for (double completed = -6.0; completed <= 12; completed += 1) {
    ASSERT_EQ(1, GDALDummyProgress(completed, "", nullptr));
  }
}

// A progress function that quits at 1.0.
int CustomProgress(const double complete, const char *message, void *arg) {
  if (complete > 1.0)
    return FALSE;
  else
    return TRUE;
}

// Tests using a custom progress handler that will request termination.
TEST(GDALProcess, ScaledProgressSimple) {
  void *progress_data = GDALCreateScaledProgress(
      0, 1, CustomProgress, nullptr);

  double completed = 0;
  for (; GDALScaledProgress(completed, "", progress_data); completed += 0.1) {}
  ASSERT_DOUBLE_EQ(1.1, completed);

  GDALDestroyScaledProgress(progress_data);
}

}  // namespace
