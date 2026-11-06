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
// Tests for GDALInfo (apps/gdalinfo_lib.cpp).
//
// Caveats:
//   GPI_HLS palette color table formatting is disabled under MSan
//   due to upstream uninitialized memory reads (TODO: b/335317901).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalinfo_lib.cpp
//   https://gdal.org/programs/gdalinfo.html

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

using GdalInfoResult = std::unique_ptr<char, CplFreeDeleter>;
using GdalInfoOptionsPtr =
    std::unique_ptr<GDALInfoOptions, decltype(&GDALInfoOptionsFree)>;

GdalInfoOptionsPtr MakeOptions(const std::vector<std::string>& args) {
  CPLStringList arg_list;
  for (const auto& arg : args) {
    arg_list.AddString(arg.c_str());
  }
  return GdalInfoOptionsPtr(
      GDALInfoOptionsNew(arg_list.List(), /*psOptionsForBinary=*/nullptr),
      GDALInfoOptionsFree);
}

GdalInfoResult RunGdalInfo(GDALDatasetH ds,
                           const GDALInfoOptions* options = nullptr) {
  return GdalInfoResult(GDALInfo(ds, options));
}

class GdalinfoLibTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
  }
};

TEST_F(GdalinfoLibTest, NullDatasetReturnsNull) {
  EXPECT_EQ(nullptr, RunGdalInfo(nullptr).get());
}

TEST_F(GdalinfoLibTest, ParserUsage) {
  const std::string usage = GDALInfoAppGetParserUsage();
  EXPECT_THAT(usage, HasSubstr("gdalinfo"));
  EXPECT_THAT(usage, HasSubstr("-json"));
  EXPECT_THAT(usage, HasSubstr("-stats"));
}

TEST_F(GdalinfoLibTest, OptionsParsingValidAndFree) {
  const std::vector<std::string> args = {
      "-json",   "-mm",       "-stats",   "-hist",   "-nogcp",
      "-nomd",   "-norat",    "-noct",    "-nofl",   "-nonodata",
      "-nomask", "-checksum", "-listmdd", "-proj4",  "-wkt_format",
      "WKT1",    "-mdd",      "all",      "-stdout",
  };
  auto opts = MakeOptions(args);
  ASSERT_NE(nullptr, opts);

  auto approx_opts = MakeOptions({"-approx_stats", "-mdd", "CustomDomain"});
  ASSERT_NE(nullptr, approx_opts);
}

TEST_F(GdalinfoLibTest, OptionsParsingWktFormats) {
  for (const char* fmt :
       {"WKT1", "WKT2", "WKT2_2015", "WKT2_2018", "WKT2_2019"}) {
    auto opts = MakeOptions({"-wkt_format", fmt});
    ASSERT_NE(nullptr, opts);
  }
}

TEST_F(GdalinfoLibTest, OptionsParsingWithBinaryOptions) {
  GDALInfoOptionsForBinary bin_options;
  CPLStringList args;
  args.AddString("-sd");
  args.AddString("1");
  args.AddString("-oo");
  args.AddString("FOO=BAR");
  args.AddString("-if");
  args.AddString("GTiff");
  args.AddString("input.tif");

  auto* opts = GDALInfoOptionsNew(args.List(), &bin_options);
  ASSERT_NE(nullptr, opts);
  GDALInfoOptionsFree(opts);

  EXPECT_EQ(1, bin_options.nSubdataset);
  EXPECT_EQ("input.tif", bin_options.osFilename);
  EXPECT_EQ(1, CSLCount(bin_options.aosOpenOptions.List()));
  EXPECT_EQ(1, CSLCount(bin_options.aosAllowedInputDrivers.List()));
}

TEST_F(GdalinfoLibTest, OptionsParsingInvalidOptionReturnsNull) {
  WithQuietHandler quiet;
  CPLErrorReset();
  CPLStringList args;
  args.AddString("-unsupported_nonexistent_flag");
  auto* opts = GDALInfoOptionsNew(args.List(), nullptr);
  EXPECT_EQ(nullptr, opts);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(GdalinfoLibTest, EmptyDatasetTextAndJson) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  // 1. Text mode default.
  {
    auto info = RunGdalInfo(ds.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Driver: MEM"));
    EXPECT_THAT(info.get(), HasSubstr("Size is 0, 0"));
  }

  // 2. JSON mode.
  {
    auto opts = MakeOptions({"-json"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"driverShortName\":\"MEM\""));
    EXPECT_THAT(info.get(), HasSubstr("\"size\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"bands\":[]"));
  }

  // 3. Suppress file list.
  {
    auto opts = MakeOptions({"-nofl"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), Not(HasSubstr("Files:")));
  }

  // 4. Stdout mode flag.
  {
    auto opts = MakeOptions({"-stdout"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
  }
}

TEST_F(GdalinfoLibTest, ProjectedDatasetWithGeoTransformAndCorners) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 10, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  const double gt[6] = {1000.0, 10.0, 0.0, 2000.0, 0.0, -10.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(const_cast<double*>(gt)));

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(32631));  // WGS 84 / UTM zone 31N
  srs.SetCoordinateEpoch(2023.5);
  ds->SetSpatialRef(&srs);

  // 1. Text mode with PROJ.4 and WKT2.
  {
    auto opts = MakeOptions({"-proj4", "-wkt_format", "WKT2"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Coordinate System is:"));
    EXPECT_THAT(info.get(), HasSubstr("WGS 84 / UTM zone 31N"));
    EXPECT_THAT(info.get(), HasSubstr("Coordinate epoch: 2023.5"));
    EXPECT_THAT(info.get(), HasSubstr("PROJ.4 string is:"));
    EXPECT_THAT(info.get(), HasSubstr("Origin = (1000"));
    EXPECT_THAT(info.get(), HasSubstr("Pixel Size = (10"));
    EXPECT_THAT(info.get(), HasSubstr("Upper Left"));
    EXPECT_THAT(info.get(), HasSubstr("Lower Left"));
    EXPECT_THAT(info.get(), HasSubstr("Upper Right"));
    EXPECT_THAT(info.get(), HasSubstr("Lower Right"));
    EXPECT_THAT(info.get(), HasSubstr("Center"));
  }

  // 2. Text mode with WKT1.
  {
    auto opts = MakeOptions({"-wkt_format", "WKT1"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("PROJCS[\"WGS 84 / UTM zone 31N\""));
  }

  // 3. JSON mode.
  {
    auto opts = MakeOptions({"-json", "-proj4", "-mm"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"geoTransform\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"cornerCoordinates\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"upperLeft\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"wgs84Extent\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"stac\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"proj:epsg\":32631"));
    EXPECT_THAT(info.get(), HasSubstr("\"proj:shape\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"proj4\":"));
  }
}

TEST_F(GdalinfoLibTest, GeographicAndLocalCoordinateSystems) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);

  // 1. Geographic SRS (EPSG:4326).
  {
    GDALDatasetUniquePtr ds(drv->Create("", 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    const double gt[6] = {10.0, 0.1, 0.0, 50.0, 0.0, -0.1};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(const_cast<double*>(gt)));

    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    ds->SetSpatialRef(&srs);

    auto info_text = RunGdalInfo(ds.get());
    ASSERT_NE(nullptr, info_text);
    EXPECT_THAT(info_text.get(), HasSubstr("GEOGCRS[\"WGS 84\""));

    auto opts_json = MakeOptions({"-json"});
    auto info_json = RunGdalInfo(ds.get(), opts_json.get());
    ASSERT_NE(nullptr, info_json);
    EXPECT_THAT(info_json.get(), HasSubstr("\"wgs84Extent\":{"));
  }

  // 2. Local Engineering SRS without projection.
  {
    GDALDatasetUniquePtr ds(drv->Create("", 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    const double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(const_cast<double*>(gt)));

    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.SetLocalCS("LocalGrid"));
    ds->SetSpatialRef(&srs);

    auto info_text = RunGdalInfo(ds.get());
    ASSERT_NE(nullptr, info_text);
    EXPECT_THAT(info_text.get(), HasSubstr("LocalGrid"));

    auto opts_json = MakeOptions({"-json"});
    auto info_json = RunGdalInfo(ds.get(), opts_json.get());
    ASSERT_NE(nullptr, info_json);
    EXPECT_THAT(info_json.get(), HasSubstr("\"proj:epsg\":null"));
  }
}

TEST_F(GdalinfoLibTest, RotatedGeoTransform) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  const double gt[6] = {100.0, 2.0, 1.0, 200.0, 1.0, -2.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(const_cast<double*>(gt)));

  // Text mode reports GeoTransform matrix.
  auto info_text = RunGdalInfo(ds.get());
  ASSERT_NE(nullptr, info_text);
  EXPECT_THAT(info_text.get(), HasSubstr("GeoTransform ="));

  // JSON mode reports cornerCoordinates and geoTransform array.
  auto opts_json = MakeOptions({"-json"});
  auto info_json = RunGdalInfo(ds.get(), opts_json.get());
  ASSERT_NE(nullptr, info_json);
  EXPECT_THAT(info_json.get(), HasSubstr("\"geoTransform\":["));
}

TEST_F(GdalinfoLibTest, GroundControlPointsReporting) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  GDAL_GCP gcps[2] = {};
  gcps[0].pszId = const_cast<char*>("gcp1");
  gcps[0].pszInfo = const_cast<char*>("info1");
  gcps[0].dfGCPPixel = 0.0;
  gcps[0].dfGCPLine = 0.0;
  gcps[0].dfGCPX = 100.0;
  gcps[0].dfGCPY = 200.0;
  gcps[0].dfGCPZ = 0.0;

  gcps[1].pszId = const_cast<char*>("gcp2");
  gcps[1].pszInfo = const_cast<char*>("info2");
  gcps[1].dfGCPPixel = 10.0;
  gcps[1].dfGCPLine = 10.0;
  gcps[1].dfGCPX = 110.0;
  gcps[1].dfGCPY = 190.0;
  gcps[1].dfGCPZ = 0.0;

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  ASSERT_EQ(CE_None, ds->SetGCPs(2, gcps, &srs));

  // 1. Text mode default.
  {
    auto info = RunGdalInfo(ds.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("GCP Projection ="));
    EXPECT_THAT(info.get(), HasSubstr("GCP[  0]: Id=gcp1, Info=info1"));
    EXPECT_THAT(info.get(), HasSubstr("GCP[  1]: Id=gcp2, Info=info2"));
  }

  // 2. Text mode -nogcp.
  {
    auto opts = MakeOptions({"-nogcp"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), Not(HasSubstr("GCP Projection =")));
    EXPECT_THAT(info.get(), Not(HasSubstr("GCP[")));
  }

  // 3. JSON mode.
  {
    auto opts = MakeOptions({"-json"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"gcps\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"gcpList\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"id\":\"gcp1\""));
    EXPECT_THAT(info.get(), HasSubstr("\"info\":\"info1\""));
  }
}

TEST_F(GdalinfoLibTest, MetadataDomainsReporting) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  ds->SetMetadataItem("DEFAULT_KEY", "DEFAULT_VAL");
  ds->SetMetadataItem("COMPRESSION", "NONE", "IMAGE_STRUCTURE");
  ds->SetMetadataItem("TILING_SCHEME_NAME", "WebMercator", "TILING_SCHEME");
  ds->SetMetadataItem("SUBDATASET_1_NAME", "sub1", "SUBDATASETS");
  ds->SetMetadataItem("GEO_KEY", "GEO_VAL", "GEOLOCATION");
  ds->SetMetadataItem("LINE_OFF", "500", "RPC");
  ds->SetMetadataItem("CLOUDCOVER", "10", "IMAGERY");
  ds->SetMetadataItem("CUSTOM_KEY", "CUSTOM_VAL", "CustomDomain");

  const char* xml_data[] = {"<root><child>value</child></root>", nullptr};
  ds->SetMetadata(const_cast<char**>(xml_data), "xml:test");

  const char* json_data[] = {"{\"key\":\"json_value\"}", nullptr};
  ds->SetMetadata(const_cast<char**>(json_data), "json:test");

  // 1. Text mode default: reports default and standard domains.
  {
    auto info = RunGdalInfo(ds.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Metadata:"));
    EXPECT_THAT(info.get(), HasSubstr("DEFAULT_KEY=DEFAULT_VAL"));
    EXPECT_THAT(info.get(), HasSubstr("Image Structure Metadata:"));
    EXPECT_THAT(info.get(), HasSubstr("Tiling Scheme:"));
    EXPECT_THAT(info.get(), HasSubstr("Subdatasets:"));
    EXPECT_THAT(info.get(), HasSubstr("Geolocation:"));
    EXPECT_THAT(info.get(), HasSubstr("RPC Metadata:"));
    EXPECT_THAT(info.get(), HasSubstr("Imagery:"));
    EXPECT_THAT(info.get(), Not(HasSubstr("CUSTOM_KEY=CUSTOM_VAL")));
  }

  // 2. Text mode -mdd CustomDomain.
  {
    auto opts = MakeOptions({"-mdd", "CustomDomain"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Metadata (CustomDomain):"));
    EXPECT_THAT(info.get(), HasSubstr("CUSTOM_KEY=CUSTOM_VAL"));
  }

  // 3. Text mode -mdd all.
  {
    auto opts = MakeOptions({"-mdd", "all"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("CUSTOM_KEY=CUSTOM_VAL"));
  }

  // 4. Text mode -listmdd.
  {
    auto opts = MakeOptions({"-listmdd"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Metadata domains:"));
    EXPECT_THAT(info.get(), HasSubstr("(default)"));
    EXPECT_THAT(info.get(), HasSubstr("CustomDomain"));
  }

  // 5. Text mode -nomd suppresses metadata.
  {
    auto opts = MakeOptions({"-nomd"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), Not(HasSubstr("DEFAULT_KEY=DEFAULT_VAL")));
    EXPECT_THAT(info.get(), Not(HasSubstr("Image Structure Metadata:")));
  }

  // 6. JSON mode: default and extra domains.
  {
    auto opts_nomd = MakeOptions({"-json", "-nomd"});
    auto info_nomd = RunGdalInfo(ds.get(), opts_nomd.get());
    ASSERT_NE(nullptr, info_nomd);
    EXPECT_THAT(info_nomd.get(), Not(HasSubstr("\"DEFAULT_KEY\"")));

    auto opts = MakeOptions({"-json", "-mdd", "all", "-listmdd"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"metadata\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"DEFAULT_KEY\":\"DEFAULT_VAL\""));
    EXPECT_THAT(info.get(), HasSubstr("\"IMAGE_STRUCTURE\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"CustomDomain\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"CUSTOM_KEY\":\"CUSTOM_VAL\""));
    EXPECT_THAT(info.get(), HasSubstr("\"xml:test\":\"<root>"));
    EXPECT_THAT(info.get(), HasSubstr("\"json:test\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"key\":\"json_value\""));
    EXPECT_THAT(info.get(), HasSubstr("\"metadataDomains\":["));
  }
}

TEST_F(GdalinfoLibTest, BandPropertiesReporting) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 4, 4, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds);

  auto* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  band->SetDescription("TestBandDescription");
  band->SetColorInterpretation(GCI_RedBand);
  band->SetUnitType("m");
  band->SetScale(2.0);
  band->SetOffset(10.0);
  band->SetNoDataValue(-999.0);

  const char* cats[] = {"Zero", "One", "Two", nullptr};
  band->SetCategoryNames(const_cast<char**>(cats));

  band->SetMetadataItem("BAND_KEY", "BAND_VAL");

  std::vector<float> data = {
      1.0f, 2.0f,  3.0f,  4.0f,  5.0f,  6.0f,  7.0f,  8.0f,
      9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f,
  };
  ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 4, 4, data.data(), 4, 4,
                                    GDT_Float32, 0, 0, nullptr));

  // 1. Text mode default.
  {
    auto info = RunGdalInfo(ds.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(),
                HasSubstr("Band 1 Block=4x1 Type=Float32, ColorInterp=Red"));
    EXPECT_THAT(info.get(), HasSubstr("Description = TestBandDescription"));
    EXPECT_THAT(info.get(), HasSubstr("NoData Value=-999"));
    EXPECT_THAT(info.get(), HasSubstr("Unit Type: m"));
    EXPECT_THAT(info.get(), HasSubstr("Offset: 10,   Scale:2"));
    EXPECT_THAT(info.get(), HasSubstr("Categories:"));
    EXPECT_THAT(info.get(), HasSubstr("0: Zero"));
    EXPECT_THAT(info.get(), HasSubstr("1: One"));
    EXPECT_THAT(info.get(), HasSubstr("BAND_KEY=BAND_VAL"));
  }

  // 2. Text mode with -mm, -stats, -checksum, -hist.
  {
    auto opts = MakeOptions({"-mm", "-stats", "-checksum", "-hist"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Computed Min/Max=1.000,16.000"));
    EXPECT_THAT(info.get(), HasSubstr("STATISTICS_MINIMUM=1"));
    EXPECT_THAT(info.get(), HasSubstr("STATISTICS_MAXIMUM=16"));
    EXPECT_THAT(info.get(), HasSubstr("STATISTICS_MEAN="));
    EXPECT_THAT(info.get(), HasSubstr("STATISTICS_STDDEV="));
    EXPECT_THAT(info.get(), HasSubstr("Checksum="));
    EXPECT_THAT(info.get(), HasSubstr("bucket"));
  }

  // 3. Text mode -approx_stats.
  {
    auto opts = MakeOptions({"-approx_stats"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("STATISTICS_MINIMUM="));
  }

  // 4. Suppress nodata with -nonodata.
  {
    auto opts = MakeOptions({"-nonodata"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), Not(HasSubstr("NoData Value=")));
  }

  // 5. JSON mode.
  {
    auto opts = MakeOptions({"-json", "-stats", "-checksum", "-hist"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"bands\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"band\":1"));
    EXPECT_THAT(info.get(), HasSubstr("\"type\":\"Float32\""));
    EXPECT_THAT(info.get(), HasSubstr("\"colorInterpretation\":\"Red\""));
    EXPECT_THAT(info.get(),
                HasSubstr("\"description\":\"TestBandDescription\""));
    EXPECT_THAT(info.get(), HasSubstr("\"noDataValue\":-999.0"));
    EXPECT_THAT(info.get(), HasSubstr("\"unit\":\"m\""));
    EXPECT_THAT(info.get(), HasSubstr("\"scale\":2.0"));
    EXPECT_THAT(info.get(), HasSubstr("\"offset\":10.0"));
    EXPECT_THAT(info.get(), HasSubstr("\"categories\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"Zero\""));
    EXPECT_THAT(info.get(), HasSubstr("\"checksum\":"));
    EXPECT_THAT(info.get(), HasSubstr("\"min\":1.0"));
    EXPECT_THAT(info.get(), HasSubstr("\"max\":16.0"));
    EXPECT_THAT(info.get(), HasSubstr("\"histogram\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"stac\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"raster:bands\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"eo:bands\":["));
  }
}

TEST_F(GdalinfoLibTest, BandInt64AndUInt64NoDataReporting) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);

  // 1. Int64 band.
  {
    GDALDatasetUniquePtr ds(drv->Create("", 2, 2, 1, GDT_Int64, nullptr));
    ASSERT_NE(nullptr, ds);
    auto* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    const int64_t nodata_val = -9223372036854775800LL;
    ASSERT_EQ(CE_None, band->SetNoDataValueAsInt64(nodata_val));

    auto info_text = RunGdalInfo(ds.get());
    ASSERT_NE(nullptr, info_text);
    EXPECT_THAT(info_text.get(), HasSubstr("-9223372036854775800"));

    auto opts_json = MakeOptions({"-json"});
    auto info_json = RunGdalInfo(ds.get(), opts_json.get());
    ASSERT_NE(nullptr, info_json);
    EXPECT_THAT(info_json.get(), HasSubstr("-9223372036854775800"));
  }

  // 2. UInt64 band.
  {
    GDALDatasetUniquePtr ds(drv->Create("", 2, 2, 1, GDT_UInt64, nullptr));
    ASSERT_NE(nullptr, ds);
    auto* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    const uint64_t nodata_val = 18446744073709551600ULL;
    ASSERT_EQ(CE_None, band->SetNoDataValueAsUInt64(nodata_val));

    auto info_text = RunGdalInfo(ds.get());
    ASSERT_NE(nullptr, info_text);
    EXPECT_THAT(info_text.get(), HasSubstr("18446744073709551600"));

    auto opts_json = MakeOptions({"-json"});
    auto info_json = RunGdalInfo(ds.get(), opts_json.get());
    ASSERT_NE(nullptr, info_json);
    EXPECT_THAT(info_json.get(), HasSubstr("18446744073709551600"));
  }
}

TEST_F(GdalinfoLibTest, ColorTableReporting) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  auto* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  band->SetColorInterpretation(GCI_PaletteIndex);

  GDALColorTable ct(GPI_RGB);
  GDALColorEntry entry0 = {0, 0, 0, 255};
  GDALColorEntry entry1 = {255, 0, 0, 255};
  ct.SetColorEntry(0, &entry0);
  ct.SetColorEntry(1, &entry1);
  ASSERT_EQ(CE_None, band->SetColorTable(&ct));

  // 1. Text mode default.
  {
    auto info = RunGdalInfo(ds.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Color Table (RGB with 2 entries)"));
    EXPECT_THAT(info.get(), HasSubstr("0: 0,0,0,255"));
    EXPECT_THAT(info.get(), HasSubstr("1: 255,0,0,255"));
  }

  // 2. Text mode -noct suppresses individual color table entries.
  {
    auto opts = MakeOptions({"-noct"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), Not(HasSubstr("0: 0,0,0,255")));
  }

  // 3. JSON mode.
  {
    auto opts = MakeOptions({"-json"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"colorTable\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"palette\":\"RGB\""));
    EXPECT_THAT(info.get(), HasSubstr("\"entries\":["));
  }

  // 4. HLS palette interpretation in JSON and text.
  // GDALInfo does not initialize GDALColorEntry if GetColorEntryAsRGB returns
  // FALSE for non-RGB palettes, causing an uninitialized memory read under
  // MSan.
#if !defined(MEMORY_SANITIZER)
  {
    GDALColorTable ct_hls(GPI_HLS);
    GDALColorEntry entry_hls = {180, 50, 50, 0};
    ct_hls.SetColorEntry(0, &entry_hls);
    ASSERT_EQ(CE_None, band->SetColorTable(&ct_hls));

    auto info_text = RunGdalInfo(ds.get());
    ASSERT_NE(nullptr, info_text);
    EXPECT_THAT(info_text.get(), HasSubstr("Color Table (HLS with 1 entries)"));

    auto opts_json = MakeOptions({"-json"});
    auto info_json = RunGdalInfo(ds.get(), opts_json.get());
    ASSERT_NE(nullptr, info_json);
    EXPECT_THAT(info_json.get(), HasSubstr("\"palette\":\"HLS\""));
  }
#endif
}

TEST_F(GdalinfoLibTest, RasterAttributeTableReporting) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  auto* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  auto rat = std::make_unique<GDALDefaultRasterAttributeTable>();
  ASSERT_EQ(CE_None,
            rat->CreateColumn("Histogram", GFT_Integer, GFU_PixelCount));
  ASSERT_EQ(CE_None, rat->CreateColumn("Class", GFT_String, GFU_Name));
  ASSERT_EQ(CE_None, rat->CreateColumn("Weight", GFT_Real, GFU_MinMax));

  rat->SetValue(0, 0, 100);
  rat->SetValue(0, 1, "Water");
  rat->SetValue(0, 2, 1.25);

  rat->SetValue(1, 0, 200);
  rat->SetValue(1, 1, "Land");
  rat->SetValue(1, 2, 2.5);

  ASSERT_EQ(CE_None, band->SetDefaultRAT(rat.get()));

  // 1. Text mode default.
  {
    auto info = RunGdalInfo(ds.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("<GDALRasterAttributeTable"));
    EXPECT_THAT(info.get(), HasSubstr("<FieldDefn index=\"0\">"));
    EXPECT_THAT(info.get(), HasSubstr("<Name>Histogram</Name>"));
    EXPECT_THAT(info.get(), HasSubstr("Water"));
    EXPECT_THAT(info.get(), HasSubstr("Land"));
  }

  // 2. Text mode -norat suppresses RAT.
  {
    auto opts = MakeOptions({"-norat"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), Not(HasSubstr("<GDALRasterAttributeTable")));
  }

  // 3. JSON mode.
  {
    auto opts = MakeOptions({"-json"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"rat\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"fieldDefn\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"name\":\"Histogram\""));
    EXPECT_THAT(info.get(), HasSubstr("\"row\":["));
  }
}

TEST_F(GdalinfoLibTest, MaskBandsReporting) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  auto* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  ASSERT_EQ(CE_None, band->CreateMaskBand(GMF_PER_DATASET));

  // 1. Text mode default.
  {
    auto info = RunGdalInfo(ds.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Mask Flags:"));
    EXPECT_THAT(info.get(), HasSubstr("PER_DATASET"));
  }

  // 2. Text mode -nomask suppresses mask.
  {
    auto opts = MakeOptions({"-nomask"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), Not(HasSubstr("Mask Flags:")));
  }

  // 3. JSON mode.
  {
    auto opts = MakeOptions({"-json"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"mask\":{"));
    EXPECT_THAT(info.get(), HasSubstr("\"flags\":["));
    EXPECT_THAT(info.get(), HasSubstr("\"PER_DATASET\""));
  }
}

TEST_F(GdalinfoLibTest, OverviewsReporting) {
  constexpr char kDir[] = "/vsimem/gdalinfo_lib_test/overviews";
  constexpr char kPath[] = "/vsimem/gdalinfo_lib_test/overviews/test.tif";

  absl::Cleanup cleanup = [kDir] { VSIRmdirRecursive(kDir); };

  auto* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);

  {
    GDALDatasetUniquePtr ds(drv->Create(kPath, 40, 40, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    auto* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    std::vector<uint8_t> pixels(40 * 40, 128);
    ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 40, 40, pixels.data(), 40,
                                      40, GDT_Byte, 0, 0, nullptr));

    int overview_list[] = {2, 4};
    ASSERT_EQ(CE_None, ds->BuildOverviews("NEAREST", 2, overview_list, 0,
                                          nullptr, nullptr, nullptr, nullptr));
  }

  GDALDatasetUniquePtr ds(GDALDataset::Open(kPath, GDAL_OF_READONLY));
  ASSERT_NE(nullptr, ds);

  // 1. Text mode default with checksum.
  {
    auto opts = MakeOptions({"-checksum"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("Overviews: 20x20, 10x10"));
    EXPECT_THAT(info.get(), HasSubstr("Overviews checksum:"));
  }

  // 2. JSON mode.
  {
    auto opts = MakeOptions({"-json", "-checksum"});
    auto info = RunGdalInfo(ds.get(), opts.get());
    ASSERT_NE(nullptr, info);
    EXPECT_THAT(info.get(), HasSubstr("\"overviews\":["));
    EXPECT_THAT(info.get(), HasSubstr("20,"));
    EXPECT_THAT(info.get(), HasSubstr("10,"));
    EXPECT_THAT(info.get(), HasSubstr("\"checksum\":"));
  }
}

TEST_F(GdalinfoLibTest, AllDataTypesStacJson) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create("", 2, 2, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  const GDALDataType types[] = {
      GDT_Int8,    GDT_UInt16, GDT_Int16,  GDT_UInt32,   GDT_Int32,
      GDT_Float64, GDT_CInt16, GDT_CInt32, GDT_CFloat32, GDT_CFloat64,
  };
  for (auto dt : types) {
    ASSERT_EQ(CE_None, ds->AddBand(dt, nullptr));
  }

  auto opts = MakeOptions({"-json"});
  auto info = RunGdalInfo(ds.get(), opts.get());
  ASSERT_NE(nullptr, info);
  EXPECT_THAT(info.get(), HasSubstr("\"data_type\":\"int8\""));
  EXPECT_THAT(info.get(), HasSubstr("\"data_type\":\"uint16\""));
  EXPECT_THAT(info.get(), HasSubstr("\"data_type\":\"int16\""));
  EXPECT_THAT(info.get(), HasSubstr("\"data_type\":\"uint32\""));
  EXPECT_THAT(info.get(), HasSubstr("\"data_type\":\"int32\""));
  EXPECT_THAT(info.get(), HasSubstr("\"data_type\":\"float64\""));
  EXPECT_THAT(info.get(), HasSubstr("\"data_type\":\"cint16\""));
  EXPECT_THAT(info.get(), HasSubstr("\"data_type\":\"cint32\""));
  EXPECT_THAT(info.get(), HasSubstr("\"data_type\":\"cfloat32\""));
  EXPECT_THAT(info.get(), HasSubstr("\"data_type\":\"cfloat64\""));
}

TEST_F(GdalinfoLibTest, MaskBandsAlphaAndNoData) {
  auto* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);

  // 1. Alpha mask
  {
    GDALDatasetUniquePtr ds(drv->Create("", 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    auto* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    ASSERT_EQ(CE_None, band->CreateMaskBand(GMF_ALPHA));

    auto info_text = RunGdalInfo(ds.get());
    ASSERT_NE(nullptr, info_text);
    EXPECT_THAT(info_text.get(), HasSubstr("ALPHA"));

    auto opts_json = MakeOptions({"-json"});
    auto info_json = RunGdalInfo(ds.get(), opts_json.get());
    ASSERT_NE(nullptr, info_json);
    EXPECT_THAT(info_json.get(), HasSubstr("\"flags\":["));
    EXPECT_THAT(info_json.get(), HasSubstr("\"ALPHA\""));
  }

  // 2. NoData mask
  {
    GDALDatasetUniquePtr ds(drv->Create("", 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    auto* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    ASSERT_EQ(CE_None, band->CreateMaskBand(GMF_NODATA | GMF_PER_DATASET));

    auto info_text = RunGdalInfo(ds.get());
    ASSERT_NE(nullptr, info_text);
    EXPECT_THAT(info_text.get(), HasSubstr("NODATA"));

    auto opts_json = MakeOptions({"-json"});
    auto info_json = RunGdalInfo(ds.get(), opts_json.get());
    ASSERT_NE(nullptr, info_json);
    EXPECT_THAT(info_json.get(), HasSubstr("\"flags\":["));
    EXPECT_THAT(info_json.get(), HasSubstr("\"NODATA\""));
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdalinfo_lib.cpp.
