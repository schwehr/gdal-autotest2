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
// Tests for GDALRasterize (apps/gdal_rasterize_lib.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdal_rasterize_lib.cpp
//   https://gdal.org/programs/gdal_rasterize.html

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class GdalRasterizeTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    RegisterOGRGeoJSON();
  }
};

std::unique_ptr<GDALDataset> CreateVectorDataset(const char* pszPath,
                                                 bool b3D = false,
                                                 bool bWithInt64 = false) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!poDriver) return nullptr;

  auto poDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(pszPath, 0, 0, 0, GDT_Unknown, nullptr));
  if (!poDS) return nullptr;

  OGRSpatialReference oSRS;
  oSRS.SetWellKnownGeogCS("WGS84");

  OGRLayer* poLayer =
      poDS->CreateLayer("shapes", &oSRS, b3D ? wkbPolygon25D : wkbPolygon);
  if (!poLayer) return nullptr;

  OGRFieldDefn oFieldVal("val", OFTReal);
  CPL_IGNORE_RET_VAL(poLayer->CreateField(&oFieldVal));

  if (bWithInt64) {
    OGRFieldDefn oFieldVal64("val64", OFTInteger64);
    CPL_IGNORE_RET_VAL(poLayer->CreateField(&oFieldVal64));
  }

  // Feature 1: polygon (2,2) to (8,8)
  {
    OGRFeature oFeat(poLayer->GetLayerDefn());
    oFeat.SetField("val", 42.0);
    if (bWithInt64) {
      oFeat.SetField("val64", static_cast<GIntBig>(10000000000LL));
    }
    auto poPoly = std::make_unique<OGRPolygon>();
    OGRLinearRing oRing;
    if (b3D) {
      oRing.addPoint(2.0, 2.0, 10.0);
      oRing.addPoint(2.0, 8.0, 20.0);
      oRing.addPoint(8.0, 8.0, 30.0);
      oRing.addPoint(8.0, 2.0, 40.0);
      oRing.addPoint(2.0, 2.0, 10.0);
    } else {
      oRing.addPoint(2.0, 2.0);
      oRing.addPoint(2.0, 8.0);
      oRing.addPoint(8.0, 8.0);
      oRing.addPoint(8.0, 2.0);
      oRing.addPoint(2.0, 2.0);
    }
    poPoly->addRing(&oRing);
    oFeat.SetGeometry(poPoly.get());
    CPL_IGNORE_RET_VAL(poLayer->CreateFeature(&oFeat));
  }

  // Feature 2: polygon (12,12) to (18,18)
  {
    OGRFeature oFeat(poLayer->GetLayerDefn());
    oFeat.SetField("val", 84.0);
    if (bWithInt64) {
      oFeat.SetField("val64", static_cast<GIntBig>(20000000000LL));
    }
    auto poPoly = std::make_unique<OGRPolygon>();
    OGRLinearRing oRing;
    if (b3D) {
      oRing.addPoint(12.0, 12.0, 50.0);
      oRing.addPoint(12.0, 18.0, 60.0);
      oRing.addPoint(18.0, 18.0, 70.0);
      oRing.addPoint(18.0, 12.0, 80.0);
      oRing.addPoint(12.0, 12.0, 50.0);
    } else {
      oRing.addPoint(12.0, 12.0);
      oRing.addPoint(12.0, 18.0);
      oRing.addPoint(18.0, 18.0);
      oRing.addPoint(18.0, 12.0);
      oRing.addPoint(12.0, 12.0);
    }
    poPoly->addRing(&oRing);
    oFeat.SetGeometry(poPoly.get());
    CPL_IGNORE_RET_VAL(poLayer->CreateFeature(&oFeat));
  }

  // Feature 3: feature without geometry
  {
    OGRFeature oFeat(poLayer->GetLayerDefn());
    oFeat.SetField("val", 0.0);
    oFeat.SetGeometry(nullptr);
    CPL_IGNORE_RET_VAL(poLayer->CreateFeature(&oFeat));
  }

  return poDS;
}

std::unique_ptr<GDALDataset> CreateRasterDataset(
    const char* pszPath, int nXSize, int nYSize, int nBands,
    GDALDataType eType = GDT_Float64) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (!poDriver) return nullptr;

  auto poDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(pszPath, nXSize, nYSize, nBands, eType, nullptr));
  if (!poDS) return nullptr;

  double adfGeoTransform[6] = {0.0, 1.0, 0.0, 20.0, 0.0, -1.0};
  poDS->SetGeoTransform(adfGeoTransform);

  OGRSpatialReference oSRS;
  oSRS.SetWellKnownGeogCS("WGS84");
  char* pszWKT = nullptr;
  oSRS.exportToWkt(&pszWKT);
  if (pszWKT) {
    poDS->SetProjection(pszWKT);
    CPLFree(pszWKT);
  }

  for (int i = 1; i <= nBands; i++) {
    GDALRasterBand* poBand = poDS->GetRasterBand(i);
    std::vector<double> zeros(nXSize * nYSize, 0.0);
    EXPECT_EQ(CE_None,
              poBand->RasterIO(GF_Write, 0, 0, nXSize, nYSize, zeros.data(),
                               nXSize, nYSize, GDT_Float64, 0, 0, nullptr));
  }

  return poDS;
}

TEST_F(GdalRasterizeTest, ParserUsage) {
  const std::string usage = GDALRasterizeAppGetParserUsage();
  EXPECT_FALSE(usage.empty());
  EXPECT_THAT(usage, HasSubstr("gdal_rasterize"));
  EXPECT_THAT(usage, HasSubstr("-burn"));
}

TEST_F(GdalRasterizeTest, OptionsBasicsAndLifecycle) {
  GDALRasterizeOptions* psOptions = GDALRasterizeOptionsNew(nullptr, nullptr);
  ASSERT_NE(nullptr, psOptions);

  GDALRasterizeOptionsSetProgress(psOptions, nullptr, nullptr);
  GDALRasterizeOptionsSetProgress(psOptions, GDALDummyProgress, nullptr);
  GDALRasterizeOptionsSetProgress(psOptions, GDALTermProgress, nullptr);

  GDALRasterizeOptionsFree(psOptions);
}

TEST_F(GdalRasterizeTest, OptionsParserValidFlags) {
  constexpr const char* apszArgv[] = {
      "-i",     "-at",       "-burn",  "100",        "-add",
      "-l",     "shapes",    "-where", "val > 10",   "-dialect",
      "OGRSQL", "-a_nodata", "0",      "-init",      "0",
      "-a_srs", "EPSG:4326", "-to",    "NAME=VALUE", "-te",
      "0",      "0",         "20",     "20",         "-tr",
      "1",      "1",         "-tap",   "-optim",     "AUTO",
      "-co",    "TILED=YES", "-ot",    "Byte",       "-of",
      "GTiff",  "-q",        nullptr,
  };

  GDALRasterizeOptions* psOptions =
      GDALRasterizeOptionsNew(const_cast<char**>(apszArgv), nullptr);
  ASSERT_NE(nullptr, psOptions);
  GDALRasterizeOptionsFree(psOptions);

  // Valid options for existing dataset (can use -b)
  constexpr const char* existingArgv[] = {
      "-b", "1", "-b", "2", "-burn", "50", "-at", nullptr,
  };
  GDALRasterizeOptions* psExistingOpt =
      GDALRasterizeOptionsNew(const_cast<char**>(existingArgv), nullptr);
  ASSERT_NE(nullptr, psExistingOpt);
  GDALRasterizeOptionsFree(psExistingOpt);
}

TEST_F(GdalRasterizeTest, OptionsParserCustomPreProcessing) {
  constexpr char kTestDir[] = "/vsimem/gdal_rasterize_test/custom_args";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  // Space-separated -burn and multiple numeric -init
  {
    constexpr const char* argv[] = {
        "-burn", "10 20 30", "-init", "1", "2", "3", "-ts", "10", "10", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALRasterizeOptionsFree(psOpt);
  }

  // Consecutive numeric -burn arguments
  {
    constexpr const char* argv[] = {
        "-burn", "10", "20", "30", "-ts", "10", "10", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALRasterizeOptionsFree(psOpt);
  }

  // Space-separated -init and space-separated -b (tested separately)
  {
    constexpr const char* argv[] = {
        "-init", "5 5", "-ts", "10", "10", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALRasterizeOptionsFree(psOpt);
  }

  {
    constexpr const char* argv[] = {
        "-b", "1 2", "-burn", "50", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALRasterizeOptionsFree(psOpt);
  }

  // Consecutive numeric -b
  {
    constexpr const char* argv[] = {
        "-b",
        "1",
        "2",
        nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALRasterizeOptionsFree(psOpt);
  }

  // Negative -a_nodata
  {
    constexpr const char* argv[] = {
        "-a_nodata", "-9999", "-ts", "10", "10", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALRasterizeOptionsFree(psOpt);
  }

  // -sql @file syntax with comments and BOM
  {
    const std::string sql_file = std::string(kTestDir) + "/query.sql";
    const std::string sql_content =
        "-- Comment line\n/* Multi-line\ncomment */\nSELECT * FROM shapes\n";
    VSIFCloseL(VSIFileFromMemBuffer(
        sql_file.c_str(),
        reinterpret_cast<GByte*>(const_cast<char*>(sql_content.c_str())),
        sql_content.size(), FALSE));

    const std::string at_sql = "@" + sql_file;
    const char* argv[] = {
        "-sql", at_sql.c_str(), "-burn", "100", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALRasterizeOptionsFree(psOpt);
  }

  // -ts with fractional dimensions (emits warning) and -outsize hidden alias
  {
    WithQuietHandler quiet;
    constexpr const char* argv[] = {
        "-ts",
        "10.5",
        "10.5",
        nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALRasterizeOptionsFree(psOpt);
  }

  {
    constexpr const char* argv[] = {
        "-outsize",
        "10",
        "10",
        nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALRasterizeOptionsFree(psOpt);
  }

  // -chunkysize hidden option (boolean flag)
  {
    constexpr const char* argv[] = {"-chunkysize", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALRasterizeOptionsFree(psOpt);
  }

  // -3d and -a
  {
    constexpr const char* argv[] = {"-3d", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALRasterizeOptionsFree(psOpt);
  }
  {
    constexpr const char* argv[] = {"-a", "val", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALRasterizeOptionsFree(psOpt);
  }
}

TEST_F(GdalRasterizeTest, OptionsParserValidationErrors) {
  WithQuietHandler quiet;

  // Unknown argument
  {
    constexpr const char* argv[] = {"-invalid_flag", nullptr};
    EXPECT_EQ(nullptr,
              GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // Invalid -a_srs
  {
    constexpr const char* argv[] = {
        "-a_srs", "INVALID_SRS_NAME_DEFINITELY_NOT_EXISTING", nullptr};
    EXPECT_EQ(nullptr,
              GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // Mutually exclusive: -burn and -3d
  {
    constexpr const char* argv[] = {"-burn", "100", "-3d", nullptr};
    EXPECT_EQ(nullptr,
              GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // Mutually exclusive: -burn and -a
  {
    constexpr const char* argv[] = {"-burn", "100", "-a", "val", nullptr};
    EXPECT_EQ(nullptr,
              GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // Mutually exclusive: -l and -sql
  {
    constexpr const char* argv[] = {"-l", "shapes", "-sql",
                                    "SELECT * FROM shapes", nullptr};
    EXPECT_EQ(nullptr,
              GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // Invalid -tr parameters (<= 0)
  {
    constexpr const char* argv[] = {"-tr", "0", "0", nullptr};
    EXPECT_EQ(nullptr,
              GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr));
  }
  {
    constexpr const char* argv[] = {"-tr", "-1", "1", nullptr};
    EXPECT_EQ(nullptr,
              GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // Invalid -ts parameters (<= 0)
  {
    constexpr const char* argv[] = {"-ts", "0", "0", nullptr};
    EXPECT_EQ(nullptr,
              GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr));
  }
  {
    constexpr const char* argv[] = {"-ts", "-1", "10", nullptr};
    EXPECT_EQ(nullptr,
              GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // bCreateOutput without -tr or -ts
  {
    constexpr const char* argv[] = {"-of", "GTiff", "-te", "0",
                                    "0",   "10",    "10",  nullptr};
    EXPECT_EQ(nullptr,
              GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // -tap without -tr
  {
    constexpr const char* argv[] = {"-tap", "-ts", "10", "10", nullptr};
    EXPECT_EQ(nullptr,
              GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // bCreateOutput with -b
  {
    constexpr const char* argv[] = {"-b",  "1",  "-of", "GTiff",
                                    "-ts", "10", "10",  nullptr};
    EXPECT_EQ(nullptr,
              GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr));
  }

  // -dialect with -where and -sql emits warning
  {
    constexpr const char* argv[] = {
        "-sql",     "SELECT * FROM shapes",
        "-where",   "val > 0",
        "-dialect", "OGRSQL",
        nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);
    GDALRasterizeOptionsFree(psOpt);
  }
}

TEST_F(GdalRasterizeTest, OptionsForBinary) {
  constexpr const char* argv[] = {
      "-burn", "100", "-of",     "GTiff",   "-ts",
      "10",    "10",  "src.shp", "dst.tif", nullptr,
  };

  GDALRasterizeOptionsForBinary sOptionsForBinary;
  GDALRasterizeOptions* psOpt =
      GDALRasterizeOptionsNew(const_cast<char**>(argv), &sOptionsForBinary);
  ASSERT_NE(nullptr, psOpt);
  EXPECT_TRUE(sOptionsForBinary.bCreateOutput);
  EXPECT_EQ("GTiff", sOptionsForBinary.osFormat);
  EXPECT_EQ("src.shp", sOptionsForBinary.osSource);
  EXPECT_EQ("dst.tif", sOptionsForBinary.osDest);
  GDALRasterizeOptionsFree(psOpt);
}

TEST_F(GdalRasterizeTest, UsageErrorsAtExecution) {
  constexpr char kTestDir[] = "/vsimem/gdal_rasterize_test/usage_errors";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  auto poSrcDS = CreateVectorDataset("src_mem");
  ASSERT_NE(nullptr, poSrcDS);

  auto poDstDS = CreateRasterDataset(
      (std::string(kTestDir) + "/dst.tif").c_str(), 10, 10, 1);
  ASSERT_NE(nullptr, poDstDS);

  WithQuietHandler quiet;
  int bUsageError = 0;

  // pszDest == nullptr && hDstDS == nullptr
  bUsageError = 0;
  EXPECT_EQ(nullptr, GDALRasterize(nullptr, nullptr,
                                   GDALDataset::ToHandle(poSrcDS.get()),
                                   nullptr, &bUsageError));
  EXPECT_EQ(1, bUsageError);

  // hSrcDataset == nullptr
  bUsageError = 0;
  EXPECT_EQ(nullptr,
            GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                          nullptr, nullptr, &bUsageError));
  EXPECT_EQ(1, bUsageError);

  // hDstDS != nullptr but options that imply creating a new dataset are set
  {
    constexpr const char* argv[] = {"-of", "GTiff", "-ts", "10", "10", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    bUsageError = 0;
    EXPECT_EQ(nullptr,
              GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                            GDALDataset::ToHandle(poSrcDS.get()), psOpt,
                            &bUsageError));
    EXPECT_EQ(1, bUsageError);
    GDALRasterizeOptionsFree(psOpt);
  }

  // Multi-layer src without -l or -sql
  {
    auto poMultiDS = std::unique_ptr<GDALDataset>(
        GetGDALDriverManager()->GetDriverByName("MEM")->Create(
            "multi_src", 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, poMultiDS);
    poMultiDS->CreateLayer("l1", nullptr, wkbPolygon);
    poMultiDS->CreateLayer("l2", nullptr, wkbPolygon);

    bUsageError = 0;
    EXPECT_EQ(nullptr,
              GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                            GDALDataset::ToHandle(poMultiDS.get()), nullptr,
                            &bUsageError));
    EXPECT_EQ(1, bUsageError);
  }

  // Output driver not recognised
  {
    const std::string out_tif = std::string(kTestDir) + "/bad_drv.tif";
    constexpr const char* argv[] = {
        "-of", "NON_EXISTENT_DRIVER", "-ts", "10", "10", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(nullptr, GDALRasterize(out_tif.c_str(), nullptr,
                                     GDALDataset::ToHandle(poSrcDS.get()),
                                     psOpt, nullptr));
    GDALRasterizeOptionsFree(psOpt);
  }

  // Output driver is not a raster driver (GeoJSON is vector only)
  {
    const std::string out_json = std::string(kTestDir) + "/bad_drv.json";
    constexpr const char* argv[] = {
        "-of", "GeoJSON", "-ts", "10", "10", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(nullptr, GDALRasterize(out_json.c_str(), nullptr,
                                     GDALDataset::ToHandle(poSrcDS.get()),
                                     psOpt, nullptr));
    GDALRasterizeOptionsFree(psOpt);
  }

  // Destination path has vector extension (.geojson) and no -of
  {
    const std::string out_vector = std::string(kTestDir) + "/out.geojson";
    constexpr const char* argv[] = {"-ts", "10", "10", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(nullptr, GDALRasterize(out_vector.c_str(), nullptr,
                                     GDALDataset::ToHandle(poSrcDS.get()),
                                     psOpt, nullptr));
    GDALRasterizeOptionsFree(psOpt);
  }
}

TEST_F(GdalRasterizeTest, ExistingDatasetRasterization) {
  constexpr char kTestDir[] = "/vsimem/gdal_rasterize_test/existing_ds";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  const std::string dst_tif = std::string(kTestDir) + "/existing.tif";
  auto poDstDS = CreateRasterDataset(dst_tif.c_str(), 20, 20, 2);
  ASSERT_NE(nullptr, poDstDS);

  auto poSrcDS = CreateVectorDataset("src_existing", /*b3D=*/true,
                                     /*bWithInt64=*/true);
  ASSERT_NE(nullptr, poSrcDS);

  // 1. Burn fixed value into band 1
  {
    constexpr const char* argv[] = {"-b", "1", "-burn", "50", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);

    // Verify burned value inside polygon (2..8, 2..8) where pixel_y is 12..18
    double dfVal = 0.0;
    GDALRasterBand* poBand = poDstDS->GetRasterBand(1);
    EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 5, 15, 1, 1, &dfVal, 1, 1,
                                        GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(50.0, dfVal);
  }

  // 2. Burn with -add and -at (all-touched) into band 1
  {
    constexpr const char* argv[] = {
        "-b", "1", "-burn", "25", "-add", "-at", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);

    double dfVal = 0.0;
    GDALRasterBand* poBand = poDstDS->GetRasterBand(1);
    EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 5, 15, 1, 1, &dfVal, 1, 1,
                                        GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(75.0, dfVal);
  }

  // 3. Burn attribute field with -a into band 2
  {
    constexpr const char* argv[] = {"-b", "2", "-a", "val", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);

    double dfVal = 0.0;
    GDALRasterBand* poBand = poDstDS->GetRasterBand(2);
    EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 5, 15, 1, 1, &dfVal, 1, 1,
                                        GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(42.0, dfVal);
  }

  // 4. Burn 3D Z values with -3d
  {
    constexpr const char* argv[] = {"-b", "1", "-3d", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);
  }

  // 5. Burn with -to transformer option on raster with no geotransform
  // (DST_METHOD=NO_GEOTRANSFORM)
  {
    auto poNoGtDS = std::unique_ptr<GDALDataset>(
        GetGDALDriverManager()->GetDriverByName("MEM")->Create(
            "no_gt_ds", 20, 20, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poNoGtDS);

    constexpr const char* argv[] = {
        "-b", "1", "-burn", "10", "-to", "SRC_METHOD=NO_GEOTRANSFORM", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(nullptr, GDALDataset::ToHandle(poNoGtDS.get()),
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);
  }

  // 6. Error: non-existent attribute field
  {
    WithQuietHandler quiet;
    constexpr const char* argv[] = {"-a", "non_existent_field", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(
        nullptr,
        GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr));
    GDALRasterizeOptionsFree(psOpt);
  }

  // 7. Error: non-existent attribute field when reprojection is active (cleans
  // up hCT)
  {
    WithQuietHandler quiet;
    auto poReprojDst = std::unique_ptr<GDALDataset>(
        GetGDALDriverManager()->GetDriverByName("MEM")->Create(
            "reproj_dst", 20, 20, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poReprojDst);
    double gt[6] = {0, 1, 0, 0, 0, -1};
    poReprojDst->SetGeoTransform(gt);
    OGRSpatialReference oSRS;
    oSRS.importFromEPSG(3857);
    char* pszWKT = nullptr;
    oSRS.exportToWkt(&pszWKT);
    poReprojDst->SetProjection(pszWKT);
    CPLFree(pszWKT);

    constexpr const char* argv[] = {"-a", "non_existent_field", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(
        nullptr,
        GDALRasterize(nullptr, GDALDataset::ToHandle(poReprojDst.get()),
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr));
    GDALRasterizeOptionsFree(psOpt);
  }

  // 8. Invalid WHERE clause error
  {
    WithQuietHandler quiet;
    constexpr const char* argv[] = {
        "-where", "SYNTAX ERROR IN WHERE CLAUSE", "-burn", "50", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(
        nullptr,
        GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr));
    GDALRasterizeOptionsFree(psOpt);
  }
}

TEST_F(GdalRasterizeTest, InvertRasterization) {
  constexpr char kTestDir[] = "/vsimem/gdal_rasterize_test/invert";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  const std::string dst_tif = std::string(kTestDir) + "/invert.tif";
  auto poDstDS = CreateRasterDataset(dst_tif.c_str(), 20, 20, 1);
  ASSERT_NE(nullptr, poDstDS);

  auto poSrcDS = CreateVectorDataset("src_invert");
  ASSERT_NE(nullptr, poSrcDS);

  // Invert mode with polygon geometries
  {
    constexpr const char* argv[] = {"-b", "1", "-burn", "100", "-i", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);

    // Pixel at (0, 0) is outside the original shapes, so in inverted mode it
    // gets burned!
    double dfVal = 0.0;
    GDALRasterBand* poBand = poDstDS->GetRasterBand(1);
    EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 0, 0, 1, 1, &dfVal, 1, 1,
                                        GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(100.0, dfVal);
  }

  // Invert mode with non-polygon geometries (warning emitted)
  {
    WithQuietHandler quiet;
    auto poPointDS = std::unique_ptr<GDALDataset>(
        GetGDALDriverManager()->GetDriverByName("MEM")->Create(
            "pts_mem", 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, poPointDS);
    OGRLayer* poLyr = poPointDS->CreateLayer("pts", nullptr, wkbPoint);
    ASSERT_NE(nullptr, poLyr);

    OGRFeature oFeat(poLyr->GetLayerDefn());
    auto poPt = std::make_unique<OGRPoint>(5.0, 5.0);
    oFeat.SetGeometry(poPt.get());
    CPL_IGNORE_RET_VAL(poLyr->CreateFeature(&oFeat));

    constexpr const char* argv[] = {"-b", "1", "-burn", "50", "-i", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                      GDALDataset::ToHandle(poPointDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);
  }

  // Invert mode with empty layer and attribute burn (no adfBurnValues)
  {
    auto poEmptyDS = std::unique_ptr<GDALDataset>(
        GetGDALDriverManager()->GetDriverByName("MEM")->Create(
            "empty_mem2", 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, poEmptyDS);
    OGRLayer* poLyr = poEmptyDS->CreateLayer("empty", nullptr, wkbPolygon);
    OGRFieldDefn oFieldVal("val", OFTReal);
    CPL_IGNORE_RET_VAL(poLyr->CreateField(&oFieldVal));

    constexpr const char* argv[] = {"-b", "1", "-a", "val", "-i", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                      GDALDataset::ToHandle(poEmptyDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);
  }
}

TEST_F(GdalRasterizeTest, CreateOutputDatasetModes) {
  constexpr char kTestDir[] = "/vsimem/gdal_rasterize_test/create_modes";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  auto poSrcDS = CreateVectorDataset("src_create", /*b3D=*/false,
                                     /*bWithInt64=*/true);
  ASSERT_NE(nullptr, poSrcDS);

  // 1. Create with -tr and -burn (automatic envelope from layer)
  {
    const std::string dst_tif = std::string(kTestDir) + "/out_tr.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-tr", "1.0", "1.0", "-burn", "100", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(dst_tif.c_str(), nullptr,
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);

    auto poOutDS = std::unique_ptr<GDALDataset>(GDALDataset::FromHandle(hRet));
    EXPECT_GT(poOutDS->GetRasterXSize(), 0);
    EXPECT_GT(poOutDS->GetRasterYSize(), 0);
  }

  // 2. Create with -te, -ts, -init, -a_nodata, and -ot Byte
  {
    const std::string dst_tif = std::string(kTestDir) + "/out_te_ts.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-te",   "0",     "0",     "20",        "20",
        "-ts", "40",    "40",    "-init", "5",     "-a_nodata", "0",
        "-ot", "Byte",  "-burn", "50",    nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(dst_tif.c_str(), nullptr,
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);

    auto poOutDS = std::unique_ptr<GDALDataset>(GDALDataset::FromHandle(hRet));
    EXPECT_EQ(40, poOutDS->GetRasterXSize());
    EXPECT_EQ(40, poOutDS->GetRasterYSize());

    GDALRasterBand* poBand = poOutDS->GetRasterBand(1);
    ASSERT_NE(nullptr, poBand);
    int bHasNoData = 0;
    const double dfNoData = poBand->GetNoDataValue(&bHasNoData);
    EXPECT_TRUE(bHasNoData);
    EXPECT_DOUBLE_EQ(0.0, dfNoData);
  }

  // 3. Create with -tap and -tr
  {
    const std::string dst_tif = std::string(kTestDir) + "/out_tap.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-tap", "-tr", "2.0", "2.0", "-burn", "75", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(dst_tif.c_str(), nullptr,
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);
    GDALClose(hRet);
  }

  // 4. Create multiple bands with single -init value (expanded across bands)
  {
    const std::string dst_tif =
        std::string(kTestDir) + "/out_multiband_single_init.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-ts", "10",    "10", "-burn",
        "10",  "20",    "30",  "-init", "0",  nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(dst_tif.c_str(), nullptr,
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);

    auto poOutDS = std::unique_ptr<GDALDataset>(GDALDataset::FromHandle(hRet));
    EXPECT_EQ(3, poOutDS->GetRasterCount());
  }

  // 5. Create with OFTInteger64 field and MEM driver (supports Int64 creation)
  {
    constexpr const char* argv[] = {
        "-of", "MEM", "-ts", "10", "10", "-a", "val64", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize("mem_int64", nullptr,
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);

    auto poOutDS = std::unique_ptr<GDALDataset>(GDALDataset::FromHandle(hRet));
    EXPECT_EQ(GDT_Int64, poOutDS->GetRasterBand(1)->GetRasterDataType());
  }

  // 6. Error: huge invalid computed raster size
  {
    WithQuietHandler quiet;
    const std::string dst_tif = std::string(kTestDir) + "/huge_size.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-te",   "0",     "0",     "10",
        "10",  "-tr",   "1e-15", "1e-15", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(nullptr, GDALRasterize(dst_tif.c_str(), nullptr,
                                     GDALDataset::ToHandle(poSrcDS.get()),
                                     psOpt, nullptr));
    GDALRasterizeOptionsFree(psOpt);
  }

  // 7. Error: empty layer without -te bounds
  {
    WithQuietHandler quiet;
    auto poEmptyDS = std::unique_ptr<GDALDataset>(
        GetGDALDriverManager()->GetDriverByName("MEM")->Create(
            "empty_ds", 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, poEmptyDS);
    poEmptyDS->CreateLayer("empty", nullptr, wkbPolygon);

    const std::string dst_tif = std::string(kTestDir) + "/empty_bounds.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-tr", "1", "1", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(nullptr, GDALRasterize(dst_tif.c_str(), nullptr,
                                     GDALDataset::ToHandle(poEmptyDS.get()),
                                     psOpt, nullptr));
    GDALRasterizeOptionsFree(psOpt);
  }

  // 8. Error: missing layer during multi-layer creation
  {
    WithQuietHandler quiet;
    const std::string dst_tif =
        std::string(kTestDir) + "/missing_lyr_create.tif";
    constexpr const char* argv[] = {
        "-of",    "GTiff", "-ts",
        "10",     "10",    "-l",
        "shapes", "-l",    "non_existent_layer",
        "-burn",  "50",    nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(nullptr, GDALRasterize(dst_tif.c_str(), nullptr,
                                     GDALDataset::ToHandle(poSrcDS.get()),
                                     psOpt, nullptr));
    GDALRasterizeOptionsFree(psOpt);
  }

  // 5. Create with OFTInteger64 field and default vs explicit -ot Int64
  {
    const std::string dst_tif = std::string(kTestDir) + "/out_int64.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-ts", "10", "10", "-a", "val64", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(dst_tif.c_str(), nullptr,
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);

    auto poOutDS = std::unique_ptr<GDALDataset>(GDALDataset::FromHandle(hRet));
    EXPECT_EQ(GDT_Float64, poOutDS->GetRasterBand(1)->GetRasterDataType());
  }

  {
    const std::string dst_tif =
        std::string(kTestDir) + "/out_explicit_int64.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-ot", "Int64", "-ts",
        "10",  "10",    "-a",  "val64", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(dst_tif.c_str(), nullptr,
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);

    auto poOutDS = std::unique_ptr<GDALDataset>(GDALDataset::FromHandle(hRet));
    EXPECT_EQ(GDT_Int64, poOutDS->GetRasterBand(1)->GetRasterDataType());
  }

  // 6. Create with -sql statement
  {
    const std::string dst_tif = std::string(kTestDir) + "/out_sql.tif";
    constexpr const char* argv[] = {
        "-of",
        "GTiff",
        "-ts",
        "10",
        "10",
        "-sql",
        "SELECT * FROM shapes WHERE val > 50",
        "-burn",
        "99",
        nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(dst_tif.c_str(), nullptr,
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);
    GDALClose(hRet);
  }

  // 7. Create with -l and -where
  {
    const std::string dst_tif = std::string(kTestDir) + "/out_where.tif";
    constexpr const char* argv[] = {
        "-of",    "GTiff",  "-ts",      "10",    "10", "-l",
        "shapes", "-where", "val < 50", "-burn", "33", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(dst_tif.c_str(), nullptr,
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);
    GDALClose(hRet);
  }

  // 8. Create with -a_srs
  {
    const std::string dst_tif = std::string(kTestDir) + "/out_srs.tif";
    constexpr const char* argv[] = {
        "-of",    "GTiff",     "-ts",   "10", "10",
        "-a_srs", "EPSG:3857", "-burn", "1",  nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(dst_tif.c_str(), nullptr,
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);

    auto poOutDS = std::unique_ptr<GDALDataset>(GDALDataset::FromHandle(hRet));
    const char* pszProj = poOutDS->GetProjectionRef();
    ASSERT_NE(nullptr, pszProj);
    EXPECT_THAT(pszProj, HasSubstr("3857"));
  }

  // 9. Error case: unwriteable destination path
  {
    WithQuietHandler quiet;
    constexpr const char* argv[] = {"-of", "GTiff", "-ts", "10", "10", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    constexpr char kNotADir[] = "/vsimem/gdal_rasterize_test/not_a_dir";
    VsiMemTempWrapper not_a_dir(kNotADir, "x");
    EXPECT_EQ(
        nullptr,
        GDALRasterize("/vsimem/gdal_rasterize_test/not_a_dir/out.tif", nullptr,
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr));
    GDALRasterizeOptionsFree(psOpt);
  }
}

TEST_F(GdalRasterizeTest, CoordinateTransformationAndWarnings) {
  constexpr char kTestDir[] = "/vsimem/gdal_rasterize_test/srs_transforms";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  // Source dataset in EPSG:4326
  auto poSrcDS = CreateVectorDataset("src_srs");
  ASSERT_NE(nullptr, poSrcDS);

  // 1. Destination raster in EPSG:3857 -> exercises coordinate transformation!
  {
    const std::string dst_tif = std::string(kTestDir) + "/reprojected.tif";
    auto poDstDS = std::unique_ptr<GDALDataset>(
        GetGDALDriverManager()->GetDriverByName("GTiff")->Create(
            dst_tif.c_str(), 100, 100, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDstDS);

    // Bounding box in Web Mercator meters for coords (0..20 deg lat/lon)
    double adfGeoTransform[6] = {0.0, 25000.0, 0.0, 2500000.0, 0.0, -25000.0};
    poDstDS->SetGeoTransform(adfGeoTransform);

    OGRSpatialReference oSRS;
    oSRS.importFromEPSG(3857);
    char* pszWKT = nullptr;
    oSRS.exportToWkt(&pszWKT);
    poDstDS->SetProjection(pszWKT);
    CPLFree(pszWKT);

    constexpr const char* argv[] = {"-b", "1", "-burn", "200", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);
  }

  // 2. Destination raster without SRS (warning emitted)
  {
    WithQuietHandler quiet;
    const std::string dst_tif = std::string(kTestDir) + "/no_srs.tif";
    auto poDstDS = std::unique_ptr<GDALDataset>(
        GetGDALDriverManager()->GetDriverByName("GTiff")->Create(
            dst_tif.c_str(), 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDstDS);
    double adfGeoTransform[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
    poDstDS->SetGeoTransform(adfGeoTransform);

    constexpr const char* argv[] = {"-b", "1", "-burn", "100", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);
  }

  // 3. Source dataset without SRS and destination raster with SRS (warning
  // emitted)
  {
    WithQuietHandler quiet;
    auto poNoSrsSrc = std::unique_ptr<GDALDataset>(
        GetGDALDriverManager()->GetDriverByName("MEM")->Create(
            "no_srs_src", 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, poNoSrsSrc);
    OGRLayer* poLyr = poNoSrsSrc->CreateLayer("shapes", nullptr, wkbPolygon);
    ASSERT_NE(nullptr, poLyr);

    OGRFeature oFeat(poLyr->GetLayerDefn());
    auto poPoly = std::make_unique<OGRPolygon>();
    OGRLinearRing oRing;
    oRing.addPoint(1.0, 1.0);
    oRing.addPoint(1.0, 9.0);
    oRing.addPoint(9.0, 9.0);
    oRing.addPoint(9.0, 1.0);
    oRing.addPoint(1.0, 1.0);
    poPoly->addRing(&oRing);
    oFeat.SetGeometry(poPoly.get());
    CPL_IGNORE_RET_VAL(poLyr->CreateFeature(&oFeat));

    const std::string dst_tif = std::string(kTestDir) + "/with_srs.tif";
    auto poDstDS = CreateRasterDataset(dst_tif.c_str(), 10, 10, 1);
    ASSERT_NE(nullptr, poDstDS);

    constexpr const char* argv[] = {"-b", "1", "-burn", "100", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                      GDALDataset::ToHandle(poNoSrsSrc.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);
  }

  // 4. Destination dataset with RPC metadata (sets WGS84 SRS)
  {
    const std::string dst_tif = std::string(kTestDir) + "/rpc.tif";
    auto poDstDS = std::unique_ptr<GDALDataset>(
        GetGDALDriverManager()->GetDriverByName("GTiff")->Create(
            dst_tif.c_str(), 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDstDS);
    double adfGeoTransform[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
    poDstDS->SetGeoTransform(adfGeoTransform);

    char* papszRPC[] = {const_cast<char*>("LINE_OFF=1"), nullptr};
    poDstDS->SetMetadata(papszRPC, "RPC");

    constexpr const char* argv[] = {"-b", "1", "-burn", "100", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet =
        GDALRasterize(nullptr, GDALDataset::ToHandle(poDstDS.get()),
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);
  }

  // 5. Geometry coordinate transformation failure (skipped in ProcessLayer)
  {
    WithQuietHandler quiet;
    auto poBadCoordDS = std::unique_ptr<GDALDataset>(
        GetGDALDriverManager()->GetDriverByName("MEM")->Create(
            "bad_coord_src", 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, poBadCoordDS);
    OGRSpatialReference oSRS;
    oSRS.SetWellKnownGeogCS("WGS84");
    OGRLayer* poLyr = poBadCoordDS->CreateLayer("shapes", &oSRS, wkbPolygon);
    ASSERT_NE(nullptr, poLyr);

    OGRFeature oFeat(poLyr->GetLayerDefn());
    auto poPoly = std::make_unique<OGRPolygon>();
    OGRLinearRing oRing;
    // Latitudes beyond 90 degrees cannot be transformed to Web Mercator
    oRing.addPoint(0.0, 95.0);
    oRing.addPoint(0.0, 96.0);
    oRing.addPoint(1.0, 96.0);
    oRing.addPoint(1.0, 95.0);
    oRing.addPoint(0.0, 95.0);
    poPoly->addRing(&oRing);
    oFeat.SetGeometry(poPoly.get());
    CPL_IGNORE_RET_VAL(poLyr->CreateFeature(&oFeat));

    const std::string dst_tif = std::string(kTestDir) + "/bad_coord_reproj.tif";
    auto poDstDS = std::unique_ptr<GDALDataset>(
        GetGDALDriverManager()->GetDriverByName("GTiff")->Create(
            dst_tif.c_str(), 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDstDS);
    double adfGeoTransform[6] = {0.0, 1000.0, 0.0, 10000.0, 0.0, -1000.0};
    poDstDS->SetGeoTransform(adfGeoTransform);
    OGRSpatialReference oWebMerc;
    oWebMerc.importFromEPSG(3857);
    char* pszWKT = nullptr;
    oWebMerc.exportToWkt(&pszWKT);
    poDstDS->SetProjection(pszWKT);
    CPLFree(pszWKT);

    constexpr const char* argv[] = {"-b", "1", "-burn", "100", nullptr};
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    GDALDatasetH hRet = GDALRasterize(
        nullptr, GDALDataset::ToHandle(poDstDS.get()),
        GDALDataset::ToHandle(poBadCoordDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);
  }
}

TEST_F(GdalRasterizeTest, ProgressAndMultipleLayers) {
  constexpr char kTestDir[] = "/vsimem/gdal_rasterize_test/progress";
  absl::Cleanup cleanup_dir = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  // Create multi-layer source
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      GetGDALDriverManager()->GetDriverByName("MEM")->Create(
          "multi_lyr_progress", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, poSrcDS);

  OGRSpatialReference oSRS;
  oSRS.SetWellKnownGeogCS("WGS84");

  OGRLayer* poLyr1 = poSrcDS->CreateLayer("lyr1", &oSRS, wkbPolygon);
  ASSERT_NE(nullptr, poLyr1);
  {
    OGRFeature oFeat(poLyr1->GetLayerDefn());
    auto poPoly = std::make_unique<OGRPolygon>();
    OGRLinearRing oRing;
    oRing.addPoint(1.0, 1.0);
    oRing.addPoint(1.0, 5.0);
    oRing.addPoint(5.0, 5.0);
    oRing.addPoint(5.0, 1.0);
    oRing.addPoint(1.0, 1.0);
    poPoly->addRing(&oRing);
    oFeat.SetGeometry(poPoly.get());
    CPL_IGNORE_RET_VAL(poLyr1->CreateFeature(&oFeat));
  }

  OGRLayer* poLyr2 = poSrcDS->CreateLayer("lyr2", &oSRS, wkbPolygon);
  ASSERT_NE(nullptr, poLyr2);
  {
    OGRFeature oFeat(poLyr2->GetLayerDefn());
    auto poPoly = std::make_unique<OGRPolygon>();
    OGRLinearRing oRing;
    oRing.addPoint(5.0, 5.0);
    oRing.addPoint(5.0, 9.0);
    oRing.addPoint(9.0, 9.0);
    oRing.addPoint(9.0, 5.0);
    oRing.addPoint(5.0, 5.0);
    poPoly->addRing(&oRing);
    oFeat.SetGeometry(poPoly.get());
    CPL_IGNORE_RET_VAL(poLyr2->CreateFeature(&oFeat));
  }

  // 1. Successful rasterization across multiple layers with progress callback
  {
    const std::string dst_tif = std::string(kTestDir) + "/multi_lyr.tif";
    constexpr const char* argv[] = {
        "-of",  "GTiff", "-ts",  "10",    "10",  "-l",
        "lyr1", "-l",    "lyr2", "-burn", "100", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    int nProgressCalls = 0;
    auto ProgressCb = [](double /*dfComplete*/, const char* /*pszMsg*/,
                         void* pProgressArg) -> int {
      int* pnCalls = static_cast<int*>(pProgressArg);
      (*pnCalls)++;
      return TRUE;
    };
    GDALRasterizeOptionsSetProgress(psOpt, ProgressCb, &nProgressCalls);

    GDALDatasetH hRet =
        GDALRasterize(dst_tif.c_str(), nullptr,
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    ASSERT_NE(nullptr, hRet);
    EXPECT_GT(nProgressCalls, 0);
    GDALClose(hRet);
  }

  // 2. Cancellation: progress callback returns FALSE
  {
    const std::string dst_tif = std::string(kTestDir) + "/cancelled.tif";
    constexpr const char* argv[] = {
        "-of", "GTiff", "-ts", "10", "10", "-l", "lyr1", "-burn", "50", nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    auto AbortProgress = [](double /*dfComplete*/, const char* /*pszMsg*/,
                            void* /*pProgressArg*/) -> int {
      return FALSE;  // Abort
    };
    GDALRasterizeOptionsSetProgress(psOpt, AbortProgress, nullptr);

    WithQuietHandler quiet;
    GDALDatasetH hRet =
        GDALRasterize(dst_tif.c_str(), nullptr,
                      GDALDataset::ToHandle(poSrcDS.get()), psOpt, nullptr);
    GDALRasterizeOptionsFree(psOpt);
    EXPECT_EQ(nullptr, hRet);
  }

  // 3. Layer not found error
  {
    WithQuietHandler quiet;
    const std::string dst_tif = std::string(kTestDir) + "/bad_lyr.tif";
    constexpr const char* argv[] = {
        "-of",   "GTiff", "-ts",   "10", "10", "-l", "non_existent_lyr",
        "-burn", "50",    nullptr,
    };
    GDALRasterizeOptions* psOpt =
        GDALRasterizeOptionsNew(const_cast<char**>(argv), nullptr);
    ASSERT_NE(nullptr, psOpt);

    EXPECT_EQ(nullptr, GDALRasterize(dst_tif.c_str(), nullptr,
                                     GDALDataset::ToHandle(poSrcDS.get()),
                                     psOpt, nullptr));
    GDALRasterizeOptionsFree(psOpt);
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdal_rasterize_lib.cpp.
