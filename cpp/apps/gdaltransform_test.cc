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
// Tests for gdaltransform CLI (apps/gdaltransform.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdaltransform.cpp
//   https://gdal.org/programs/gdaltransform.html

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/apps/gdaltransform_shim.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::autotest2_gdaltransform::RunGdalTransformMain;
using ::testing::HasSubstr;
using ::testing::NotNull;

// Feeds input to stdin via an in-memory pipe.
// Constraint: Single-threaded execution only; input size must fit in Linux pipe
// buffer (< 64 KiB).
class StdinFeeder {
 public:
  explicit StdinFeeder(const std::string& input) {
    EXPECT_LT(input.size(), 65536u);
    saved_stdin_ = dup(STDIN_FILENO);
    int pipe_fds[2];
    if (pipe(pipe_fds) == 0) {
      size_t written = 0;
      while (written < input.size()) {
        ssize_t n =
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

void EnsureDriversRegistered() {
  GDALRegister_MEM();
  GDALRegister_GTiff();
}

std::unique_ptr<GDALDataset> CreateGeoTiffWithSrs(const std::string& path,
                                                  int epsg_code) {
  EnsureDriversRegistered();
  auto* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (!drv) return nullptr;
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create(path.c_str(), 10, 10, 1, GDT_Byte, nullptr));
  if (!ds) return nullptr;
  OGRSpatialReference srs;
  srs.importFromEPSG(epsg_code);
  ds->SetSpatialRef(&srs);
  double gt[6] = {1000.0, 1.0, 0.0, 2000.0, 0.0, -1.0};
  ds->SetGeoTransform(gt);
  ds->FlushCache(false);
  return ds;
}

TEST(GdaltransformTest, UsageAndHelpFlags) {
  EXPECT_EQ(0, RunGdalTransformMain({"gdaltransform", "--help"}));
  EXPECT_EQ(0, RunGdalTransformMain({"gdaltransform", "--help-doc"}));
  EXPECT_EQ(0, RunGdalTransformMain({"gdaltransform", "--utility_version"}));

  {
    WithQuietHandler quiet;
    // Unknown option
    EXPECT_EQ(1, RunGdalTransformMain({"gdaltransform", "-unknown_flag"}));
    // Too many command arguments
    EXPECT_EQ(
        1, RunGdalTransformMain({"gdaltransform", "file1", "file2", "file3"}));

    // Missing option arguments
    EXPECT_EQ(1, RunGdalTransformMain({"gdaltransform", "-s_srs"}));
    EXPECT_EQ(1, RunGdalTransformMain({"gdaltransform", "-t_srs"}));
    EXPECT_EQ(1, RunGdalTransformMain({"gdaltransform", "-s_coord_epoch"}));
    EXPECT_EQ(1, RunGdalTransformMain({"gdaltransform", "-t_coord_epoch"}));
    EXPECT_EQ(1, RunGdalTransformMain({"gdaltransform", "-ct"}));
    EXPECT_EQ(1, RunGdalTransformMain({"gdaltransform", "-order"}));
    EXPECT_EQ(1, RunGdalTransformMain({"gdaltransform", "-to"}));
    EXPECT_EQ(1,
              RunGdalTransformMain({"gdaltransform", "-gcp", "0", "0", "10"}));

    // Invalid SRS definitions
    EXPECT_EQ(1, RunGdalTransformMain(
                     {"gdaltransform", "-s_srs", "INVALID_SRS_XYZ"}));
    EXPECT_EQ(1, RunGdalTransformMain(
                     {"gdaltransform", "-t_srs", "INVALID_SRS_XYZ"}));
  }
}

TEST(GdaltransformTest, BasicCoordTransform) {
  // 2D coordinate transform
  EXPECT_EQ(
      0, RunGdalTransformMain({"gdaltransform", "-s_srs", "EPSG:4326", "-t_srs",
                               "EPSG:3857", "-coord", "10", "20"}));

  // 3D coordinate transform (with Z)
  EXPECT_EQ(
      0, RunGdalTransformMain({"gdaltransform", "-s_srs", "EPSG:4326", "-t_srs",
                               "EPSG:3857", "-coord", "10", "20", "50"}));

  // 4D coordinate transform (with Z and T)
  EXPECT_EQ(0, RunGdalTransformMain({"gdaltransform", "-s_srs", "EPSG:4326",
                                     "-t_srs", "EPSG:3857", "-coord", "10",
                                     "20", "50", "2020.5"}));

  // Inverse transformation (-i)
  EXPECT_EQ(0, RunGdalTransformMain({"gdaltransform", "-i", "-s_srs",
                                     "EPSG:4326", "-t_srs", "EPSG:3857",
                                     "-coord", "1113194.9", "2273030.9"}));

  // Output formatting: -output_xy, -E, field separators
  EXPECT_EQ(0, RunGdalTransformMain({"gdaltransform", "-output_xy", "-s_srs",
                                     "EPSG:4326", "-t_srs", "EPSG:3857",
                                     "-coord", "10", "20"}));
  EXPECT_EQ(
      0, RunGdalTransformMain({"gdaltransform", "-E", "-s_srs", "EPSG:4326",
                               "-t_srs", "EPSG:3857", "-coord", "10", "20"}));
  EXPECT_EQ(0, RunGdalTransformMain({"gdaltransform", "-E", "-output_xy",
                                     "-s_srs", "EPSG:4326", "-t_srs",
                                     "EPSG:3857", "-coord", "10", "20"}));

  // Field separators including escaped forms (\t, \r, \n)
  EXPECT_EQ(0, RunGdalTransformMain({"gdaltransform", "-field_sep", ",",
                                     "-s_srs", "EPSG:4326", "-t_srs",
                                     "EPSG:3857", "-coord", "10", "20"}));
  EXPECT_EQ(0, RunGdalTransformMain({"gdaltransform", "-field_sep", "\\t",
                                     "-s_srs", "EPSG:4326", "-t_srs",
                                     "EPSG:3857", "-coord", "10", "20"}));
  EXPECT_EQ(0, RunGdalTransformMain({"gdaltransform", "-field_sep", "\\r",
                                     "-s_srs", "EPSG:4326", "-t_srs",
                                     "EPSG:3857", "-coord", "10", "20"}));
  EXPECT_EQ(0, RunGdalTransformMain({"gdaltransform", "-field_sep", "\\n",
                                     "-s_srs", "EPSG:4326", "-t_srs",
                                     "EPSG:3857", "-coord", "10", "20"}));

  // Coordinate epochs
  EXPECT_EQ(0, RunGdalTransformMain({"gdaltransform", "-s_srs", "EPSG:4326",
                                     "-t_srs", "EPSG:3857", "-s_coord_epoch",
                                     "2020.0", "-t_coord_epoch", "2021.0",
                                     "-coord", "10", "20"}));

  // Coordinate operation (-ct)
  EXPECT_EQ(
      0,
      RunGdalTransformMain(
          {"gdaltransform", "-s_srs", "EPSG:4326", "-t_srs", "EPSG:3857", "-ct",
           "+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad "
           "+step +proj=webmerc",
           "-coord", "10", "20"}));

  // Transformer options (-to)
  EXPECT_EQ(
      0, RunGdalTransformMain({"gdaltransform", "-s_srs", "EPSG:4326", "-t_srs",
                               "EPSG:3857", "-to", "SRC_METHOD=NO_GEOTRANSFORM",
                               "-coord", "10", "20"}));
}

TEST(GdaltransformTest, DatasetTransforms) {
  constexpr char kDir[] = "/vsimem/gdaltransform_test";
  constexpr char kSrcPath[] = "/vsimem/gdaltransform_test/src.tif";
  constexpr char kDstPath[] = "/vsimem/gdaltransform_test/dst.tif";

  absl::Cleanup clean_dir = [kDir] {
    EnsureDriversRegistered();
    VSIRmdirRecursive(kDir);
  };

  ASSERT_THAT(CreateGeoTiffWithSrs(kSrcPath, 4326), NotNull());
  ASSERT_THAT(CreateGeoTiffWithSrs(kDstPath, 3857), NotNull());

  // Single dataset: pixel/line to georeferenced coordinates
  EXPECT_EQ(
      0, RunGdalTransformMain({"gdaltransform", kSrcPath, "-coord", "0", "0"}));

  // Filename following 2 coordinates (covers CPLGetValueType ==
  // CPL_VALUE_STRING for Z)
  EXPECT_EQ(
      0, RunGdalTransformMain({"gdaltransform", "-coord", "0", "0", kSrcPath}));

  // Filename following 3 coordinates (covers CPLGetValueType ==
  // CPL_VALUE_STRING for T)
  EXPECT_EQ(0, RunGdalTransformMain(
                   {"gdaltransform", "-coord", "0", "0", "10", kSrcPath}));

  // Two datasets: source to destination
  EXPECT_EQ(0, RunGdalTransformMain(
                   {"gdaltransform", kSrcPath, kDstPath, "-coord", "0", "0"}));

  {
    WithQuietHandler quiet;
    // Error: Non-existent source dataset
    EXPECT_EQ(
        1, RunGdalTransformMain({"gdaltransform",
                                 "/vsimem/gdaltransform_test/nonexistent.tif",
                                 "-coord", "0", "0"}));

    // Error: Non-existent destination dataset
    EXPECT_EQ(
        1, RunGdalTransformMain({"gdaltransform", kSrcPath,
                                 "/vsimem/gdaltransform_test/nonexistent.tif",
                                 "-coord", "0", "0"}));

    // Error: Both dataset and GCPs specified
    EXPECT_EQ(1, RunGdalTransformMain({"gdaltransform", kSrcPath, "-gcp", "0",
                                       "0", "100", "200", "-coord", "0", "0"}));
  }
}

TEST(GdaltransformTest, GcpAndTpsTransforms) {
  // GCPs with 4 args and 5 args (both non-zero elevation and zero elevation)
  // Followed by non-numeric token (-coord) to exercise endptr branch
  const std::vector<std::string> gcp_args = {"gdaltransform",
                                             "-gcp",
                                             "0",
                                             "0",
                                             "100",
                                             "200",
                                             "15.5",
                                             "-gcp",
                                             "0",
                                             "10",
                                             "100",
                                             "210",
                                             "0",
                                             "-gcp",
                                             "10",
                                             "0",
                                             "110",
                                             "200",
                                             "0.0",
                                             "-gcp",
                                             "10",
                                             "10",
                                             "110",
                                             "210",
                                             "20.0",
                                             "-gcp",
                                             "5",
                                             "5",
                                             "105",
                                             "205",
                                             "-order",
                                             "1",
                                             "-coord",
                                             "5",
                                             "5"};
  EXPECT_EQ(0, RunGdalTransformMain(gcp_args));

  // GCPs with order 2
  std::vector<std::string> gcp_order2_args = {"gdaltransform",
                                              "-gcp",
                                              "0",
                                              "0",
                                              "100",
                                              "200",
                                              "-gcp",
                                              "0",
                                              "5",
                                              "100",
                                              "205",
                                              "-gcp",
                                              "0",
                                              "10",
                                              "100",
                                              "210",
                                              "-gcp",
                                              "5",
                                              "0",
                                              "105",
                                              "200",
                                              "-gcp",
                                              "5",
                                              "5",
                                              "105",
                                              "205",
                                              "-gcp",
                                              "10",
                                              "0",
                                              "110",
                                              "200",
                                              "-order",
                                              "2",
                                              "-coord",
                                              "5",
                                              "5"};
  EXPECT_EQ(0, RunGdalTransformMain(gcp_order2_args));

  // GCPs with TPS (-tps)
  std::vector<std::string> tps_args = {
      "gdaltransform", "-tps", "-gcp",   "0",  "0",
      "100",           "200",  "-gcp",   "0",  "10",
      "100",           "210",  "-gcp",   "10", "0",
      "110",           "200",  "-gcp",   "10", "10",
      "110",           "210",  "-coord", "5",  "5"};
  EXPECT_EQ(0, RunGdalTransformMain(tps_args));

  // RPC and Geoloc options
  EXPECT_EQ(
      0, RunGdalTransformMain({"gdaltransform", "-rpc", "-s_srs", "EPSG:4326",
                               "-t_srs", "EPSG:3857", "-coord", "10", "20"}));
  EXPECT_EQ(0, RunGdalTransformMain({"gdaltransform", "-geoloc", "-s_srs",
                                     "EPSG:4326", "-t_srs", "EPSG:3857",
                                     "-coord", "10", "20"}));
}

TEST(GdaltransformTest, StdinInputHandling) {
  // Test reading points from stdin:
  // 1. Line with < 2 tokens (prints "Not enough values at line 1")
  // 2. Line with 2 tokens
  // 3. Line with 3 tokens (string extra comment)
  // 4. Line with 3 tokens (number Z)
  // 5. Line with 4 tokens (string extra comment)
  // 6. Line with 4 tokens (number T)
  // 7. Line with 5+ tokens (Z, T, extra comments)
  // 8. CRLF line endings
  // 9. Coordinate epoch variation (dfT != dfLastT)
  const std::string stdin_data =
      "single_token\n"
      "10 20\n"
      "10 20 comment_text\n"
      "10 20 35.5\n"
      "10 20 35.5 extra_note\n"
      "10 20 35.5 2020.5\n"
      "10 20 35.5 2020.5 extra comment here\n"
      "15 25\r\n"
      "10 20 0 2020.5\n"
      "10 20 0 0\n";

  {
    StdinFeeder feeder(stdin_data);
    EXPECT_EQ(0, RunGdalTransformMain({"gdaltransform", "-s_srs", "EPSG:4326",
                                       "-t_srs", "EPSG:3857"}));
  }

  // With -ignore_extra_input
  {
    const std::string extra_data = "10 20 extra input ignored\n";
    StdinFeeder feeder(extra_data);
    EXPECT_EQ(0, RunGdalTransformMain({"gdaltransform", "-ignore_extra_input",
                                       "-s_srs", "EPSG:4326", "-t_srs",
                                       "EPSG:3857"}));
  }
}

TEST(GdaltransformTest, FailureBranches) {
  {
    WithQuietHandler quiet;

    // Out-of-bounds coordinate (latitude 95 in Web Mercator fails
    // transformation) Exercises line 495: printf("transformation failed.\n");
    ::testing::internal::CaptureStdout();
    EXPECT_EQ(
        0, RunGdalTransformMain({"gdaltransform", "-s_srs", "EPSG:4326",
                                 "-t_srs", "EPSG:3857", "-coord", "0", "95"}));
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_THAT(out, HasSubstr("transformation failed"));

    // Transformer creation failure (order 2 polynomial requires >= 6 GCPs; 4
    // GCPs causes nullptr) Exercises line 352-355: if (hTransformArg ==
    // nullptr) exit(1);
    EXPECT_EQ(1, RunGdalTransformMain({"gdaltransform",
                                       "-gcp",
                                       "0",
                                       "0",
                                       "100",
                                       "200",
                                       "-gcp",
                                       "0",
                                       "10",
                                       "100",
                                       "210",
                                       "-gcp",
                                       "10",
                                       "0",
                                       "110",
                                       "200",
                                       "-gcp",
                                       "10",
                                       "10",
                                       "110",
                                       "210",
                                       "-order",
                                       "2",
                                       "-coord",
                                       "0",
                                       "0"}));
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdaltransform.cpp.
