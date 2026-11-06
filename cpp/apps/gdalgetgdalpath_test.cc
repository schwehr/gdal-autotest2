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
// Tests for GDALGetGDALPath (apps/gdalgetgdalpath.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalgetgdalpath.cpp

#include "third_party/gdal/apps/gdalgetgdalpath.h"

#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>  // NOLINT
#include <fstream>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kVsiDir[] = "/vsimem/gdalgetgdalpath_test";

TEST(GdalGetGdalPathTest, GdalPathConfigOptionFound) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kVsiDir); };
  ASSERT_EQ(0, VSIMkdir(kVsiDir, 0755));

  const std::string fake_bin = std::string(kVsiDir) + "/gdal";
  VSILFILE* fp = VSIFOpenL(fake_bin.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  EXPECT_EQ(0, VSIFCloseL(fp));

  const CPLConfigOptionSetter setter("GDAL_PATH", kVsiDir, false);
  EXPECT_EQ(fake_bin, GDALGetGDALPath());
}

TEST(GdalGetGdalPathTest, GdalPathConfigOptionNotFound) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kVsiDir); };
  ASSERT_EQ(0, VSIMkdir(kVsiDir, 0755));

  const CPLConfigOptionSetter setter("GDAL_PATH", kVsiDir, false);
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ("", GDALGetGDALPath());
  }
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("No 'gdal' binary can be found"));
  CPLErrorReset();
}

TEST(GdalGetGdalPathTest, SpawnGdalInPathSuccess) {
  const CPLConfigOptionSetter setter("GDAL_PATH", nullptr, false);

  const auto temp_bin_dir =
      std::filesystem::path(::testing::TempDir()) / "gdalpath_success";
  std::filesystem::create_directories(temp_bin_dir);
  absl::Cleanup cleanup_dir = [&] {
    std::filesystem::remove_all(temp_bin_dir);
  };

  const auto script_path = temp_bin_dir / "gdal";
  {
    std::ofstream ofs(script_path);
    ofs << "#!/bin/sh\n";
    ofs << "printf '%s\\n' '" << GDALVersionInfo("") << "'\n";
  }
  ASSERT_EQ(0, chmod(script_path.c_str(), 0755));

  const char* orig_path = getenv("PATH");
  const std::string saved_path = orig_path ? orig_path : "";
  absl::Cleanup restore_path = [saved_path] {
    if (saved_path.empty()) {
      unsetenv("PATH");
    } else {
      setenv("PATH", saved_path.c_str(), 1);
    }
  };

  setenv("PATH", temp_bin_dir.c_str(), 1);
  EXPECT_EQ("gdal", GDALGetGDALPath()) << CPLGetLastErrorMsg();
}

TEST(GdalGetGdalPathTest, SpawnGdalInPathWrongVersion) {
  const CPLConfigOptionSetter setter("GDAL_PATH", nullptr, false);

  const auto temp_bin_dir =
      std::filesystem::path(::testing::TempDir()) / "gdalpath_wrong_ver";
  std::filesystem::create_directories(temp_bin_dir);
  absl::Cleanup cleanup_dir = [&] {
    std::filesystem::remove_all(temp_bin_dir);
  };

  const auto script_path = temp_bin_dir / "gdal";
  {
    std::ofstream ofs(script_path);
    ofs << "#!/bin/sh\n";
    ofs << "echo \"GDAL 0.0.0, released 1970/01/01\"\n";
  }
  ASSERT_EQ(0, chmod(script_path.c_str(), 0755));

  const char* orig_path = getenv("PATH");
  const std::string saved_path = orig_path ? orig_path : "";
  absl::Cleanup restore_path = [saved_path] {
    if (saved_path.empty()) {
      unsetenv("PATH");
    } else {
      setenv("PATH", saved_path.c_str(), 1);
    }
  };

  setenv("PATH", temp_bin_dir.c_str(), 1);

  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ("", GDALGetGDALPath());
  }
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("returned 'GDAL 0.0.0"));
  CPLErrorReset();
}

TEST(GdalGetGdalPathTest, SpawnGdalNotFoundInPath) {
  const CPLConfigOptionSetter setter("GDAL_PATH", nullptr, false);

  const auto temp_empty_dir =
      std::filesystem::path(::testing::TempDir()) / "gdalpath_empty";
  std::filesystem::create_directories(temp_empty_dir);
  absl::Cleanup cleanup_dir = [&] {
    std::filesystem::remove_all(temp_empty_dir);
  };

  const char* orig_path = getenv("PATH");
  const std::string saved_path = orig_path ? orig_path : "";
  absl::Cleanup restore_path = [saved_path] {
    if (saved_path.empty()) {
      unsetenv("PATH");
    } else {
      setenv("PATH", saved_path.c_str(), 1);
    }
  };

  setenv("PATH", temp_empty_dir.c_str(), 1);

  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ("", GDALGetGDALPath());
  }
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Could not find 'gdal' binary"));
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdalgetgdalpath.cpp.
