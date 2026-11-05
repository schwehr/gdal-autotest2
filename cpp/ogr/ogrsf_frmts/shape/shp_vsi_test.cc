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

#include "third_party/gdal/ogr/ogrsf_frmts/shape/shp_vsi.h"

#include <climits>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/shapelib/shapefil.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::Ne;
using ::testing::NotNull;

TEST(ShpVsiTest, SetupDefaultHooks) {
  SAHooks hooks;
  SASetupDefaultHooks(&hooks);

  EXPECT_THAT(hooks.FOpen, NotNull());
  EXPECT_THAT(hooks.FRead, NotNull());
  EXPECT_THAT(hooks.FWrite, NotNull());
  EXPECT_THAT(hooks.FSeek, NotNull());
  EXPECT_THAT(hooks.FTell, NotNull());
  EXPECT_THAT(hooks.FFlush, NotNull());
  EXPECT_THAT(hooks.FClose, NotNull());
  EXPECT_THAT(hooks.Remove, NotNull());
  EXPECT_THAT(hooks.Error, NotNull());
  EXPECT_THAT(hooks.Atof, NotNull());
  EXPECT_EQ(hooks.pvUserData, nullptr);

  EXPECT_DOUBLE_EQ(hooks.Atof("123.45"), 123.45);
}

TEST(ShpVsiTest, GetHookDifferences) {
  const SAHooks* h0 = VSI_SHP_GetHook(0);
  const SAHooks* h1 = VSI_SHP_GetHook(1);

  ASSERT_THAT(h0, NotNull());
  ASSERT_THAT(h1, NotNull());
  EXPECT_THAT(h0, Ne(h1));
  EXPECT_THAT(h0->FOpen, Ne(h1->FOpen));
  EXPECT_EQ(h0->FRead, h1->FRead);
  EXPECT_EQ(h0->FWrite, h1->FWrite);
  EXPECT_EQ(h0->FSeek, h1->FSeek);
  EXPECT_EQ(h0->FTell, h1->FTell);
  EXPECT_EQ(h0->FFlush, h1->FFlush);
  EXPECT_EQ(h0->FClose, h1->FClose);
  EXPECT_EQ(h0->Remove, h1->Remove);
  EXPECT_EQ(h0->Error, h1->Error);
  EXPECT_EQ(h0->Atof, h1->Atof);
  EXPECT_DOUBLE_EQ(h0->Atof("42.5"), 42.5);
}

TEST(ShpVsiTest, OpenNonExistentFails) {
  const SAHooks* h0 = VSI_SHP_GetHook(0);
  const SAHooks* h1 = VSI_SHP_GetHook(1);

  EXPECT_EQ(h0->FOpen("/vsimem/does_not_exist_123.shp", "rb", nullptr),
            nullptr);
  EXPECT_EQ(h1->FOpen("/vsimem/does_not_exist_123.shp", "rb", nullptr),
            nullptr);
}

TEST(ShpVsiTest, ReadWriteSeekTellFlushClose) {
  constexpr const char* kFilename = "/vsimem/shp_vsi_rw_test.bin";
  const SAHooks* hook = VSI_SHP_GetHook(0);

  SAFile file = hook->FOpen(kFilename, "wb+", nullptr);
  ASSERT_THAT(file, NotNull());
  auto cleanup = absl::MakeCleanup([kFilename, hook, &file]() {
    if (file != nullptr) {
      hook->FClose(file);
      file = nullptr;
    }
    VSIUnlink(kFilename);
  });

  EXPECT_STREQ(VSI_SHP_GetFilename(file), kFilename);
  EXPECT_THAT(VSI_SHP_GetVSIL(file), NotNull());
  EXPECT_EQ(hook->FTell(file), 0);

  const std::string content = "GDAL Shape VSI test data buffer";
  EXPECT_EQ(hook->FWrite(content.data(), 1, content.size(), file),
            content.size());
  EXPECT_EQ(hook->FTell(file), content.size());
  EXPECT_EQ(hook->FFlush(file), 0);

  // Seek relative to beginning (whence = 0)
  EXPECT_EQ(hook->FSeek(file, 5, 0), 0);
  EXPECT_EQ(hook->FTell(file), 5);

  // Seek relative to current (whence = 1)
  EXPECT_EQ(hook->FSeek(file, 3, 1), 0);
  EXPECT_EQ(hook->FTell(file), 8);

  // Seek relative to end (whence = 2)
  EXPECT_EQ(hook->FSeek(file, 0, 2), 0);
  EXPECT_EQ(hook->FTell(file), content.size());

  // Seek back to start and read
  EXPECT_EQ(hook->FSeek(file, 0, 0), 0);
  std::vector<char> buffer(content.size() + 1, 0);
  EXPECT_EQ(hook->FRead(buffer.data(), 1, content.size(), file),
            content.size());
  EXPECT_STREQ(buffer.data(), content.c_str());
  EXPECT_EQ(hook->FTell(file), content.size());

  EXPECT_EQ(hook->FClose(file), 0);
  file = nullptr;
}

TEST(ShpVsiTest, RemoveFile) {
  constexpr const char* kFilename = "/vsimem/shp_vsi_remove_test.bin";
  const SAHooks* hook = VSI_SHP_GetHook(0);

  SAFile file = hook->FOpen(kFilename, "wb", nullptr);
  ASSERT_THAT(file, NotNull());
  hook->FClose(file);

  EXPECT_EQ(hook->Remove(kFilename, nullptr), 0);
  EXPECT_NE(hook->Remove(kFilename, nullptr), 0);
}

TEST(ShpVsiTest, ErrorHook) {
  const SAHooks* hook = VSI_SHP_GetHook(0);
  WithQuietHandler quiet_error;
  hook->Error("Custom error via VSI_SHP_Error");
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Custom error via VSI_SHP_Error"));
}

TEST(ShpVsiTest, WriteMoreDataOkEnforce2GB) {
  constexpr const char* kFilename = "/vsimem/shp_vsi_2gb_limit_test.bin";
  const SAHooks* hook2GB = VSI_SHP_GetHook(1);

  SAFile file = hook2GB->FOpen(kFilename, "wb+", nullptr);
  ASSERT_THAT(file, NotNull());
  auto cleanup = absl::MakeCleanup([kFilename, hook2GB, &file]() {
    if (file != nullptr) {
      hook2GB->FClose(file);
      file = nullptr;
    }
    VSIUnlink(kFilename);
  });

  // Normal write is allowed
  EXPECT_TRUE(VSI_SHP_WriteMoreDataOK(file, 1024));

  // Write exceeding 2GB with limit enforced fails with error
  WithQuietHandler quiet_error;
  constexpr SAOffset kHugeOffset = static_cast<SAOffset>(INT_MAX) + 10;
  EXPECT_FALSE(VSI_SHP_WriteMoreDataOK(file, kHugeOffset));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("2GB file size limit reached"));

  // Attempting to write through hook fails immediately because WriteMoreDataOK
  // fails
  char dummy = 'A';
  EXPECT_EQ(hook2GB->FWrite(&dummy, 1, kHugeOffset, file), 0);
}

TEST(ShpVsiTest, WriteMoreDataOkWarn2GB) {
  constexpr const char* kFilename = "/vsimem/shp_vsi_2gb_warn_test.bin";
  const SAHooks* hookNoLimit = VSI_SHP_GetHook(0);

  SAFile file = hookNoLimit->FOpen(kFilename, "wb+", nullptr);
  ASSERT_THAT(file, NotNull());
  auto cleanup = absl::MakeCleanup([kFilename, hookNoLimit, &file]() {
    if (file != nullptr) {
      hookNoLimit->FClose(file);
      file = nullptr;
    }
    VSIUnlink(kFilename);
  });

  EXPECT_TRUE(VSI_SHP_WriteMoreDataOK(file, 512));

  // First call exceeding 2GB produces warning
  WithQuietHandler quiet_error;
  constexpr SAOffset kHugeOffset = static_cast<SAOffset>(INT_MAX) + 100;
  CPLErrorReset();
  EXPECT_TRUE(VSI_SHP_WriteMoreDataOK(file, kHugeOffset));
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Going on, but might cause compatibility issues"));

  // Second call does not warn again
  CPLErrorReset();
  EXPECT_TRUE(VSI_SHP_WriteMoreDataOK(file, kHugeOffset));
  EXPECT_STREQ(CPLGetLastErrorMsg(), "");
}

}  // namespace
}  // namespace autotest2
