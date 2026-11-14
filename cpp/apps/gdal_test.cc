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
// Tests for the unified gdal CLI front-end (apps/gdal.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdal.cpp
//   https://gdal.org/programs/index.html

#include "third_party/gdal/gcore/gdal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/apps/gdal_shim.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::autotest2_gdal::RunGdalMain;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::NotNull;

// Feeds input to stdin via an in-memory pipe.
// Single-threaded execution only; input size must fit in pipe buffer.
class StdinFeeder {
 public:
  explicit StdinFeeder(const std::string& input) {
    EXPECT_LT(input.size(), 65536u);
    saved_stdin_ = dup(STDIN_FILENO);
    int pipe_fds[2];
    if (pipe(pipe_fds) == 0) {
      size_t written = 0;
      while (written < input.size()) {
        const ssize_t n =
            write(pipe_fds[1], input.data() + written, input.size() - written);
        if (n <= 0) break;
        written += n;
      }
      close(pipe_fds[1]);
      dup2(pipe_fds[0], STDIN_FILENO);
      close(pipe_fds[0]);
      clearerr(stdin);
    }
  }

  ~StdinFeeder() {
    if (saved_stdin_ != -1) {
      dup2(saved_stdin_, STDIN_FILENO);
      close(saved_stdin_);
      clearerr(stdin);
    }
  }

  StdinFeeder(const StdinFeeder&) = delete;
  StdinFeeder& operator=(const StdinFeeder&) = delete;

 private:
  int saved_stdin_ = -1;
};

void CreateTinyTiff(const std::string& path, uint8_t fill_val = 42) {
  GDALRegister_GTiff();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(drv, NotNull());
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_THAT(ds, NotNull());
  std::vector<uint8_t> pixels(16, fill_val);
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, pixels.data(),
                                           4, 4, GDT_Byte, 0, 0, nullptr));
}

TEST(GdalCliTest, GeneralCmdLineOptions) {
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", "--version"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("GDAL"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", "--formats"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("GTiff"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", "--format", "GTiff"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("GeoTIFF"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", "--help-general"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("--version"));
  }
  {
    WithQuietHandler quiet;
    EXPECT_EQ(1, RunGdalMain({"gdal", "--config"}));
  }
}

TEST(GdalCliTest, DriversHelpAndJsonUsage) {
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", "--drivers"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("short_name"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", "--help"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("Usage: gdal"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", "--json-usage"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("\"name\""));
  }
}

TEST(GdalCliTest, ConfigOptionsInStdin) {
  StdinFeeder feeder("--config\nGDAL_TEST_STDIN_OPT=val%201\nEND_CONFIG\n");
  ::testing::internal::CaptureStdout();
  const int ret =
      RunGdalMain({"gdal", "--config-options-in-stdin", "--version"});
  const std::string out = ::testing::internal::GetCapturedStdout();
  EXPECT_EQ(0, ret);
  EXPECT_THAT(out, HasSubstr("GDAL"));
  EXPECT_EQ(nullptr, CPLGetConfigOption("GDAL_TEST_STDIN_OPT", nullptr));
}

TEST(GdalCliTest, CompletionMode) {
  // 1. Subcommand completion with last_word_is_complete=true and false.
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain(
        {"gdal", "completion", "gdal", "raster", "last_word_is_complete=true"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("info"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain(
        {"gdal", "completion", "gdal", "ra", "last_word_is_complete=false"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("raster"));
  }

  // 2. --config completion branches.
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain(
        {"gdal", "completion", "gdal", "raster", "info", "--config"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("GDAL_CACHEMAX="));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain(
        {"gdal", "completion", "gdal", "raster", "info", "--config="});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("GDAL_CACHEMAX="));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain(
        {"gdal", "completion", "gdal", "raster", "info", "--config", "foo="});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, IsEmpty());
  }

  // 3. prev= / cur= bash completion branches (unique, duplicate, none, '=').
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", "completion", "gdal", "raster", "info",
                                 "--fo", "prev=info", "cur=--fo"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("--format"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", "completion", "gdal", "raster",
                                 "raster", "prev=gdal", "cur=raster"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, Not(IsEmpty()));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", "completion", "gdal", "raster",
                                 "prev=gdal", "cur=nonexistent_token"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("info"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", "completion", "gdal", "raster", "info",
                                 "--format=GTiff", "prev==", "cur=GTiff"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, Not(IsEmpty()));
  }
}

TEST(GdalCliTest, ShortFormAndPipelineExecution) {
  constexpr char kDir[] = "/vsimem/gdal_cli_test_short";
  constexpr char kTifPath[] = "/vsimem/gdal_cli_test_short/in.tif";
  constexpr char kOutTifPath[] = "/vsimem/gdal_cli_test_short/out.tif";
  constexpr char kGeoJsonPath[] = "/vsimem/gdal_cli_test_short/pts.geojson";
  absl::Cleanup clean_dir = [kDir] { VSIRmdirRecursive(kDir); };

  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(kTifPath));
  VsiMemTempWrapper geojson(
      kGeoJsonPath,
      R"json({"type":"FeatureCollection","features":[{"type":"Feature","geometry":{"type":"Point","coordinates":[1,2]},"properties":{"id":1}}]})json");

  // 1. Short-form "gdal <in.tif>".
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", kTifPath});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("Driver: GTiff/GeoTIFF"));
  }

  // 2. Short-form "gdal <in.geojson>".
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", kGeoJsonPath});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("using driver `GeoJSON'"));
  }

  // 3. Short-form pipeline "gdal read <in.tif> ! info".
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", "read", kTifPath, "!", "info"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("\"driverShortName\":\"GTiff\""));
  }

  // 4. Forwarded --format flag in subcommand (argc != 3 workaround).
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", "info", "--format", "json", kTifPath});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("\"driverShortName\":\"GTiff\""));
  }

  // 5. Subcommand with --progress and output dataset verification.
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain(
        {"gdal", "raster", "convert", "--progress", kTifPath, kOutTifPath});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("done."));

    GDALRegister_GTiff();
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(kOutTifPath, GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(4, out_ds->GetRasterXSize());
    EXPECT_EQ(4, out_ds->GetRasterYSize());
  }
}

TEST(GdalCliTest, ReturnCodeAndErrors) {
  constexpr char kDir[] = "/vsimem/gdal_cli_test_err";
  constexpr char kTifA[] = "/vsimem/gdal_cli_test_err/a.tif";
  constexpr char kTifB[] = "/vsimem/gdal_cli_test_err/b.tif";
  absl::Cleanup clean_dir = [kDir] { VSIRmdirRecursive(kDir); };

  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(kTifA, 10));
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(kTifB, 20));

  // 1. Output "return-code" argument from "gdal raster compare" on differing
  // datasets.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalMain({"gdal", "raster", "compare", kTifA, kTifB});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_GT(ret, 0);
    EXPECT_THAT(out, Not(IsEmpty()));
  }

  // 2. ParseCommandLineArguments failure printing usage to stderr.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalMain({"gdal", "--unknown_option_xyz"});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("Usage: gdal"));
  }

  // 3. ParseCommandLineArguments failure with typo suggestion suppresses usage.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalMain({"gdal", "rastr"});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, IsEmpty());
  }

  // 4. ParseCommandLineArguments failure with "Should be one among" suppresses
  // usage.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret =
        RunGdalMain({"gdal", "info", "--output-format", "bogus", kTifA});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, IsEmpty());
  }

  // 5. Runtime error returns 1.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(
        1, RunGdalMain({"gdal", "info", "/vsimem/gdal_cli_test_err/none.tif"}));
  }
}

}  // namespace
}  // namespace autotest2
