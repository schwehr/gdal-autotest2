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
// Tests for gdalinfo CLI binary (apps/gdalinfo_bin.cpp).
//
// Caveats:
//   --help/-h/--long-usage call ::std::exit(0) inside GDALArgumentParser in
//   //third_party/gdal and cannot be caught by the binary shim without process
//   exit; general options and Usage() are tested via --help-general and invalid
//   arguments instead.
//   Archive listing is exercised via a custom non-leaking VSIFilesystemHandler
//   rather than VSIArchiveFilesystemHandler::IsArchive(), which leaks the char*
//   returned by SplitFilename() in upstream GDAL.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalinfo_bin.cpp
//   https://gdal.org/programs/gdalinfo.html

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>  // NOLINT
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/apps/gdalinfo_bin_shim.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

using ::autotest2_gdalinfo_bin::RunGdalInfoMain;
using ::testing::HasSubstr;
using ::testing::NotNull;

void CreateTinyGeoTiff(const std::string& path, int width = 8, int height = 8,
                       uint8_t fill_val = 42) {
  GDALRegister_GTiff();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(drv, NotNull());
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), width, height, 1, GDT_Byte, nullptr));
  ASSERT_THAT(ds, NotNull());
  double gt[6] = {10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  ASSERT_EQ(CE_None, ds->SetSpatialRef(&srs));
  std::vector<uint8_t> pixels(width * height, fill_val);
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, width, height, pixels.data(), width,
                         height, GDT_Byte, 0, 0, nullptr));
}

constexpr char kFakeArchivePrefix[] = "/vsifake_gdalinfo_archive/";
constexpr char kSmallArchive[] = "/vsifake_gdalinfo_archive/small.zip";
constexpr char kLargeArchive[] = "/vsifake_gdalinfo_archive/large.zip";
constexpr char kUnopenableArchive[] =
    "/vsifake_gdalinfo_archive/unopenable.zip";

class FakeArchiveDir final : public VSIDIR {
 public:
  struct Entry {
    std::string name;
    bool is_dir = false;
  };

  explicit FakeArchiveDir(std::vector<Entry> entries)
      : entries_(std::move(entries)) {}

  const VSIDIREntry* NextDirEntry() override {
    if (index_ >= entries_.size()) {
      return nullptr;
    }
    CPLFree(current_.pszName);
    current_.pszName = CPLStrdup(entries_[index_].name.c_str());
    current_.nMode = entries_[index_].is_dir ? S_IFDIR : S_IFREG;
    current_.bModeKnown = 1;
    ++index_;
    return &current_;
  }

 private:
  std::vector<Entry> entries_;
  size_t index_ = 0;
  VSIDIREntry current_{};
};

class FakeArchiveFSHandler final : public VSIFilesystemHandler {
 public:
  VSIVirtualHandleUniquePtr Open(const char* /*pszFilename*/,
                                 const char* /*pszAccess*/, bool /*bSetError*/,
                                 CSLConstList /*papszOptions*/) override {
    return nullptr;
  }

  int Stat(const char* /*pszFilename*/, VSIStatBufL* /*pStatBuf*/,
           int /*nFlags*/) override {
    return -1;
  }

  bool IsArchive(const char* pszPath) const override {
    return std::string_view(pszPath).starts_with(kFakeArchivePrefix);
  }

  VSIDIR* OpenDir(const char* pszPath, int /*nRecurseDepth*/,
                  const char* const* /*papszOptions*/) override {
    const std::string_view path(pszPath);
    if (path == kSmallArchive) {
      return new FakeArchiveDir({
          {.name = "top.txt", .is_dir = false},
          {.name = "subdir", .is_dir = true},
          {.name = "subdir_slash/", .is_dir = true},
      });
    }
    if (path == kLargeArchive) {
      std::vector<FakeArchiveDir::Entry> entries;
      entries.reserve(105);
      for (int i = 0; i < 105; ++i) {
        entries.push_back(
            {.name = std::format("f_{:03d}.txt", i), .is_dir = false});
      }
      return new FakeArchiveDir(std::move(entries));
    }
    return nullptr;
  }
};

TEST(GdalinfoBinTest, GeneralCmdLineOptionsAndDebugExit) {
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalInfoMain({"gdalinfo", "--version"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("GDAL"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalInfoMain({"gdalinfo", "--formats"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("GTiff"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalInfoMain({"gdalinfo", "--format", "GTiff"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("GeoTIFF"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalInfoMain({"gdalinfo", "--help-general"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("--version"));
  }
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalInfoMain({"gdalinfo", "--debug", "ON", "--version"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("GDAL"));
  }
  {
    WithQuietHandler quiet;
    EXPECT_EQ(1, RunGdalInfoMain({"gdalinfo", "--config"}));
  }
}

TEST(GdalinfoBinTest, InvalidArgsAndMissingDataset) {
  constexpr char kDir[] = "/vsimem/gdalinfo_bin_errors";
  absl::Cleanup clean_dir = [kDir] { VSIRmdirRecursive(kDir); };

  // 1. Missing dataset argument triggers Usage() -> exit 1.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalInfoMain({"gdalinfo"});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("Usage: gdalinfo"));
  }

  // 2. Unknown option triggers Usage() -> exit 1.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalInfoMain({"gdalinfo", "--unknown_opt"});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("Usage: gdalinfo"));
  }

  // 3. Non-existent dataset exits 1.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret =
        RunGdalInfoMain({"gdalinfo", "/vsimem/gdalinfo_bin_errors/none.tif"});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("gdalinfo failed - unable to open"));
  }

  // 4. Existing vector dataset suggests ogrinfo (gdalinfo_bin uses VSIStat,
  // which only checks real POSIX files, not /vsimem/).
  const std::filesystem::path geojson_path =
      std::filesystem::path(::testing::TempDir()) / "gdalinfo_bin_vec.geojson";
  absl::Cleanup clean_geojson = [&geojson_path] {
    std::error_code ec;
    std::filesystem::remove(geojson_path, ec);
  };
  {
    constexpr std::string_view kGeoJsonContent =
        R"json({"type":"FeatureCollection","features":[{"type":"Feature","geometry":{"type":"Point","coordinates":[0,0]},"properties":{}}]})json";
    VSILFILE* fp = VSIFOpenL(geojson_path.string().c_str(), "wb");
    ASSERT_THAT(fp, NotNull());
    absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
    ASSERT_EQ(kGeoJsonContent.size(), VSIFWriteL(kGeoJsonContent.data(), 1,
                                                 kGeoJsonContent.size(), fp));
  }
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalInfoMain({"gdalinfo", geojson_path.string()});
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("Did you intend to call ogrinfo?"));
  }
}

TEST(GdalinfoBinTest, ArchiveListingAndTrimming) {
  auto handler = std::make_unique<FakeArchiveFSHandler>();
  VSIFileManager::InstallHandler(kFakeArchivePrefix, handler.get());
  absl::Cleanup remove_handler = [] {
    VSIFileManager::RemoveHandler(kFakeArchivePrefix);
  };

  // 1. Small archive with regular file, dir without trailing '/', and dir with
  //    trailing '/'.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStdout();
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalInfoMain({"gdalinfo", kSmallArchive});
    const std::string out = ::testing::internal::GetCapturedStdout();
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("unable to open"));
    EXPECT_THAT(out, HasSubstr("The archive contains several files:"));
    EXPECT_THAT(out, HasSubstr("/small.zip/top.txt\n"));
    EXPECT_THAT(out, HasSubstr("/small.zip/subdir/\n"));
    EXPECT_THAT(out, HasSubstr("/small.zip/subdir_slash/\n"));
  }

  // 2. Large archive with > 100 entries exercises [...trimmed...] truncation.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStdout();
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalInfoMain({"gdalinfo", kLargeArchive});
    const std::string out = ::testing::internal::GetCapturedStdout();
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("unable to open"));
    EXPECT_THAT(out, HasSubstr("[...trimmed...]"));
  }

  // 3. Unopenable archive where VSIOpenDir returns nullptr.
  {
    WithQuietHandler quiet;
    ::testing::internal::CaptureStdout();
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalInfoMain({"gdalinfo", kUnopenableArchive});
    const std::string out = ::testing::internal::GetCapturedStdout();
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(1, ret);
    EXPECT_THAT(err, HasSubstr("unable to open"));
    EXPECT_EQ("", out);
  }
}

TEST(GdalinfoBinTest, BasicInfoJsonAndSubdatasets) {
  constexpr char kDir[] = "/vsimem/gdalinfo_bin_basic";
  constexpr char kTif[] = "/vsimem/gdalinfo_bin_basic/test.tif";
  constexpr char kVrt[] = "/vsimem/gdalinfo_bin_basic/container.vrt";
  absl::Cleanup clean_dir = [kDir] { VSIRmdirRecursive(kDir); };

  ASSERT_NO_FATAL_FAILURE(CreateTinyGeoTiff(kTif, 8, 8, 42));

  // 1. Default text output (auto-appends -stdout).
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalInfoMain({"gdalinfo", kTif});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("Driver: GTiff/GeoTIFF"));
    EXPECT_THAT(out, HasSubstr("Size is 8, 8"));
  }

  // 2. Explicit -stdout and -json -stats -mm -checksum.
  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalInfoMain(
        {"gdalinfo", "-stdout", "-json", "-stats", "-mm", "-checksum", kTif});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("\"driverShortName\":\"GTiff\""));
    EXPECT_THAT(out, HasSubstr("\"minimum\":42.0"));
  }

  // 3. Subdataset selection (-sd 1) and out-of-range warning (-sd 99).
  const std::string vrt_xml = std::format(
      "<VRTDataset rasterXSize=\"8\" rasterYSize=\"8\">\n"
      "  <Metadata domain=\"SUBDATASETS\">\n"
      "    <MDI key=\"SUBDATASET_1_NAME\">{}</MDI>\n"
      "    <MDI key=\"SUBDATASET_1_DESC\">First subdataset</MDI>\n"
      "  </Metadata>\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\"/>\n"
      "</VRTDataset>\n",
      kTif);
  VsiMemTempWrapper vrt_file(kVrt, vrt_xml);

  {
    ::testing::internal::CaptureStdout();
    const int ret = RunGdalInfoMain({"gdalinfo", "-sd", "1", kVrt});
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(out, HasSubstr("Driver: GTiff/GeoTIFF"));
    EXPECT_THAT(out, HasSubstr("Size is 8, 8"));
  }

  {
    ::testing::internal::CaptureStdout();
    ::testing::internal::CaptureStderr();
    const int ret = RunGdalInfoMain({"gdalinfo", "-sd", "99", kVrt});
    const std::string out = ::testing::internal::GetCapturedStdout();
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(0, ret);
    EXPECT_THAT(err, HasSubstr("subdataset 99 of 1 requested"));
    EXPECT_THAT(out, HasSubstr("Driver: VRT/Virtual Raster"));
  }
}

}  // namespace
}  // namespace autotest2
